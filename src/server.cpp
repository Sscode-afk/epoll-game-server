#include "server/server.h"
#include "server/socket.h"
#include "server/connection.h"
#include "core/log.h"
#include <errno.h>
#include "server/serversettings.h"
#include <fcntl.h>
#include <arpa/inet.h>
#include <memory>
#include <cstring>
#include "proto/parser.h"
#include "proto/wire.h"
#include "core/tools.h"
#include "server/corestructures.h"
#include "core/time.h"
#include <algorithm>

//POLICY: MEMORY ALLOCATION FAILURES ARE LEFT UNHANDLED
server::server() {
    epollfd = -1;
    listenerfd = -1;
    clientsid = 1;
    listenerdisabled = false;
    authconncount = 0;
    unauthconncount = 0;

    reservefd = -1;

    lastipsweeptime = now();
}

server::~server() {
    if (epollfd >= 0) close(epollfd);
    if (listenerfd >= 0) close(listenerfd);
    if (reservefd >= 0) close(reservefd);
}

bool server::init() {
    if ((reservefd = open("/dev/null",O_RDONLY)) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"fcntl reserve open() err!");
        return false;
    }
    if ((listenerfd = getlistenerfd()) == -1) {
        return false;
    }

    if ((epollfd = epoll_create1(0)) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum, "epoll create err!");
        return false;
    }
    
    epoll_event listenerevent;
    listenerevent.events = EPOLLIN;
    listenerevent.data.ptr = nullptr;

    if (epoll_ctl(epollfd,EPOLL_CTL_ADD,listenerfd,&listenerevent) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"listener epoll_ctl_add err!");
        return false;
    }
    LOGINFO("Listenerfd (%d) and Epollfd(%d) set, server init completed.",listenerfd,epollfd);
    return true;
}

void server::run() {
    LOGINFO("Running the server...!");
    while(true) {
        epoll_event eventbuffer[serverfields::EPEVENTbufsize];

        int events;
        if ((events = epoll_wait(epollfd,eventbuffer,serverfields::EPEVENTbufsize,-1)) == -1) {
            int errnum = errno;
            LOGERRORNUM(errnum,"epoll wait err!");
            return;
        }

        for (int i = 0; i < events; i++) {
            epoll_event& event = eventbuffer[i];
            int processresult = processevents(event);
            if (processresult == -1) return;
        }

        reap();
    }
}

//THE DESIGN ALLOWS THE FOLLOWING - AN EVENT IN A BATCH WILL HAVE ITS CONNECTION
//IN THE MAP AS LONG ATLEAST UNTIL ITS ENTIRE BATCH HAS NOT BEEN PROCESSED FIRST
//thus event raw pointers are NEVER stale
int server::processevents(epoll_event& event) {
    connection * currentconn = static_cast<connection *>(event.data.ptr);

    if (currentconn!= nullptr && currentconn->alive == false) return 0;
    if (event.events & (EPOLLERR | EPOLLHUP)) {
        if (currentconn == nullptr) {
            int errnum = errno;
            LOGERRORNUM(errnum,"Listner error!");
            return -1; //listener error, fatal
        }
        handlemessage(currentconn); //handle any pending messages
        markdead(currentconn);
        return 0;
    }
    if (event.events & EPOLLIN) {
        if (currentconn == nullptr) {
            //listener event, ready to accept
            sockaddr_in theiraddr;
            socklen_t theirsize;
            while (true) {
                theirsize = sizeof theiraddr;
                int newfd = accept(listenerfd,(sockaddr *)&theiraddr,&theirsize);

                if (newfd == -1) {
                    int errnum = errno;
                    if (errnum == EAGAIN || errnum == EWOULDBLOCK) {
                        break;
                    }
                    else if (errnum == EINTR || errnum == ECONNABORTED) {
                        LOGINFO("accept() dropped client!");
                        continue;
                    }
                    else if (errnum == EMFILE || errnum == ENFILE) {
                        LOGWARNINGNUM(errnum,"Fd limit reached, cleaning accept buffer!");

                        if (reservefd!=-1) {
                            close(reservefd);
                            int toclean = accept(listenerfd,nullptr,nullptr);
                            if (toclean!=-1) close(toclean);

                            reservefd = open("/dev/null",O_RDONLY);
                        }

                        if (reservefd == -1) {
                            int errnum_a = errno;
                            LOGWARNINGNUM(errnum_a,"Could not reopen reserve FD, disabling EPOLLIN on listener for now!");
                            
                            epoll_event ev;
                            ev.events = 0;
                            ev.data.ptr = nullptr;

                            if (epoll_ctl(epollfd,EPOLL_CTL_MOD,listenerfd,&ev) == -1) {
                                int errnum_b = errno;
                                LOGERRORNUM(errnum_b,"Could not disable listener EPOLLIN, shutting down!");
                                return -1;
                            }

                            listenerdisabled = true; //EPOLL will be enabled when a client disconnects
                        }

                        break;
                    }
                    else if (errnum == ENOMEM || errnum == ENOBUFS || errnum == EPROTO || errnum == EPERM) {
                        //keep the server alive for the healthy clients but stopping the accept look
                        LOGERRORNUM(errnum,"Transient network/system error, breaking accept loop!");
                        break;
                    }
                    else {
                        //bad listener, fatal
                        LOGERRORNUM(errnum,"Accept() err!");
                        return -1;
                    }
                }

                else {
                    SERVERASSERT(unauthconncount <= serverfields::MAXunauthconnlimit);
                    if (unauthconncount == serverfields::MAXunauthconnlimit) {
                        char addrbuf[INET_ADDRSTRLEN];
                        fillIP(addrbuf,sizeof addrbuf,theiraddr);
                        LOGINFO("Maximum unauthorized connection count reached! dropping client(%s)!",addrbuf);
                        //2k clients already in the pool
                        close(newfd);
                        continue;
                    }

                    //check for unauthorized concurrent connections per IP count
                    uint32_t connip = static_cast<uint32_t>(theiraddr.sin_addr.s_addr);
                    auto [it,insert] = ipmap.try_emplace(connip,nullptr);

                    if (insert) it->second = std::make_unique<essential::IP>(now());

                    essential::IP * ippointer = it->second.get();
                    //does not affect new IP, since tokens are already at maximum
                    essential::updatetokens(ippointer->tb,now());

                    //first check affordability and then only deduct
                    if (ippointer->tb.tokens < 1 || ippointer->count == serverfields::MAXipconcurrentusers) {
                        //deduct on failure too, since it cost server work
                        ippointer->tb.tokens = std::clamp(ippointer->tb.tokens - serverfields::accepttokencost,0.0,ippointer->tb.capacity);
                        close(newfd);
                        continue;
                    }
                    ippointer->tb.tokens = std::clamp(ippointer->tb.tokens - serverfields::accepttokencost,0.0,ippointer->tb.capacity);

                    if (setnonblocking(newfd) == false) {
                        close(newfd);
                        continue;
                    }

                    epoll_event newclient;
                    newclient.events = EPOLLIN;
                    
                    auto newconn = std::make_unique<connection>(); //uniqueptr

                    newconn->alive = true;
                    newconn->connaddr = theiraddr;
                    newconn->polloutactive = false;
                    newconn->connfd = newfd;
                    newconn->connid = clientsid;
                    newconn->readindex = 0;
                    newconn->writeindex = 0;
                    
                    newconn->lastseen = now();

                    filltag(newconn->tag,sizeof newconn->tag,newconn->connaddr,newconn->connid);
                    //we get the raw pointer to the connection object for the event that acts as an observer
                    //deletion etc. is upto the smart pointer, and this is done before moving it to the map

                    connection * c = newconn.get();
                    newclient.data.ptr = c;

                    LOGINFO("accepted new client! (%s)",newconn->tag);

                    if (epoll_ctl(epollfd,EPOLL_CTL_ADD,newfd,&newclient) == -1) {
                        int errnum = errno;
                        LOGWARNINGNUM(errnum,"epoll add failed for client(%s), dropping client!",newconn->tag);
                        
                        //transfer to map occurs only if this succeeds, failure leads to the connection pointer going out of scope
                        //this triggers connection's destructor which closes the fd.
                        continue;
                    }

                    ippointer->count += 1;
                    //shifted to post ctl add success to prevent unauthcount from leaking
                    changestate(newconn.get(),serverfields::connstates::UNAUTH);
                    unauthqueue.push(clientsid);

                    auto [connit,conninserted] = connectionsmap.try_emplace(clientsid,nullptr);
                    SERVERASSERT(conninserted == true); //since clientsid is incremented per client, for a new conn, the entry cannot exist
                    connit->second = std::move(newconn);
                    clientsid++;
                }
            }
            
        }

        else {
            handlemessage(currentconn);
        }
    }
    if (event.events & EPOLLOUT) {
        //epollout logic here
    }

    return 0;
}

void server::handlemessage(connection * c) {
    if (c->alive == false) return;

    while (true) {
        size_t remaining = serverfields::CLIENTREADbufsizemax - c->writeindex;

        if (remaining == 0) {
            //the read buffer has filled up, break
            break;
        }

        ssize_t bytes = recv(c->connfd,c->readbuffer + c->writeindex,remaining,0);

        if (bytes == -1) {
            int errnum = errno;
            if (errnum == EAGAIN || errnum == EWOULDBLOCK) {
                break;
            }
            else {
                LOGWARNINGNUM(errnum,"recv() failure on client (%s)!",c->tag);
                markdead(c);
                return;
            }
        }
        else if (bytes == 0) {
            //FIN packet sent by client
            LOGINFO("client(%s) closed their connection (FIN)!",c->tag);
            markdead(c);
            return;
        }

        else {
            //the bytes are written to the read buffer, updating the write index
            c->writeindex += bytes;
        }
    }

    //now the buffer is ready to be read
    while (true) {
        //A HANDLER MUST RETURN -1 TO DROP THE CLIENT HOLDS
        //in the odd case it doesn't (handler marks client dead and returns 0), above handles
        if (!c->alive) return;

        int parseresult = parse(c->readbuffer,c->readindex,c->writeindex,c->tag);
        if (parseresult == -2) break;

        else if (parseresult == -1) {
            markdead(c);
            return;
        }

        //parse result dictates execution control as well as the actual type of the msg
        int status = 0;
        switch(parseresult) {
            case 1 : {
                status = handleauth();
                break;
            }
            default : {
                //unreachable for healthy client -- BADTYPE
                LOGWARNING("Unknown message type for %s, dropping!",c->tag);
                markdead(c);
                return;
            }
        }

        if (status == -1) {
            markdead(c);
            return;
        }
    }

    if (c->readindex == c->writeindex) {
        c->readindex = 0;
        c->writeindex = 0;
    }

    if (c->readindex > 0) {
        //this branch will run if incomplete messages are present in the readbuffer.
        size_t tomove = c->writeindex - c->readindex;
        memmove(c->readbuffer,c->readbuffer+c->readindex,tomove);
        c->readindex = 0;
        c->writeindex = tomove;
    }

}
//MARKDEAD NEVER DELETES, just sets alive to false
//Deletion only happens in toreap(). any method during processevents always gets a valid raw pointer
void server::markdead(connection * c) {
    if (c->alive == false) return; //some previous event in the batch already marked this client dead
    c->alive = false;
    //decrement IP count
    auto pit = ipmap.find(c->connaddr.sin_addr.s_addr);
    SERVERASSERT(pit!=ipmap.end()); //IP counts are only decremented in markdead, and incremented on accept
    //markdead called on a connection MUST have its corresponding ipmap entry alive as non zero count does not reap it

    pit->second->count -= 1;
    //evaluate tokens before checking
    essential::updatetokens(pit->second->tb,now());
    if (pit->second->count == 0 && pit->second->tb.tokens == pit->second->tb.capacity) ipmap.erase(pit); //oppurtunistic early cleanup
    toremove.push_back(c->connid);
    changestate(c,serverfields::connstates::DEAD);

    LOGINFO("Client(%s) marked dead!",c->tag);
}

void server::reap() {

    uint64_t unauthnow = now();
    while (!unauthqueue.empty()) {
        uint64_t& id = unauthqueue.front();
        auto it = connectionsmap.find(id);

        if (it == connectionsmap.end()) {
            //the connection is already dead. this is possible for clients that were sitting
            //back in the queue, and they could not come to the front of the queue fast enough
            //in a reap cycle in which they were removed (considering queue operation happens
            //before removal)!
            unauthqueue.pop();
        }
        else if (it->second->alive == false) {
            unauthqueue.pop(); //someone marked the client dead and they are to be removed in this
            //reap cycle!
        }
        else if (it->second->state != serverfields::connstates::UNAUTH) {
            unauthqueue.pop();
        }
        else if ((unauthnow - it->second->lastseen) > serverfields::authtimeout) {
            markdead(it->second.get());
            unauthqueue.pop();
        }
        else break; //if an unauth client has not timed out, nobody after him has!
    }
    //ORDERING MATTERS ABOVE! LASTSEEN ACCESS IN QUEUE IS ONLY FOR UNAUTH CONNECTIONS
    //THE SAME FIELD IS USED FOR UPDATING DEADLINE OF AUTH CLIENTS!

    //IF connections will be cleared and listener is disabled
    if (!toremove.empty() && listenerdisabled) {
        epoll_event ev;
        ev.data.ptr = nullptr;
        ev.events = EPOLLIN;
        int enablelistener = epoll_ctl(epollfd,EPOLL_CTL_MOD,listenerfd,&ev);

        SERVERASSERT(enablelistener!=-1);
        listenerdisabled = false;

        LOGINFO("Connection(s) dropped, re-enabled EPOLLIN on listenerfd!");
    }

    for (auto& id:toremove) {
        connectionsmap.erase(id); //connection destructor closes fd
    }
    toremove.clear();

    //safety check, moved to bottom because markdead decrements that unauth/auth counts
    //eagerly. originally at the top is would abort instantly since connections were not removed
    //above IP sweep so that the ASSERT fires every reap cycle
    SERVERASSERT(static_cast<size_t>(authconncount + unauthconncount) == connectionsmap.size());

    if (now() - lastipsweeptime < serverfields::ipsweepinterval) return;
    lastipsweeptime = now();
    //the following sweep is for IP whose connection count reached 0 but at the moment
    //of markdead, their tokens had not refilled due to heavy use, they get swept periodically
    for (auto it = ipmap.begin(); it!=ipmap.end();) {
        essential::updatetokens(it->second->tb,now());
        if (it->second->count == 0 && it->second->tb.tokens == it->second->tb.capacity) {
            it = ipmap.erase(it);
        }
        else {
            it++;
        }
    }
}

void server::changestate(connection * conn,serverfields::connstates newstate) {
    serverfields::connstates tochange = conn->state;
    if (tochange == serverfields::connstates::NONE && newstate == serverfields::connstates::UNAUTH) {
        conn->state = newstate;
        unauthconncount++;
    }
    else if (tochange == serverfields::connstates::UNAUTH && newstate == serverfields::connstates::IDLE) {
        conn->state = newstate;
        unauthconncount--;
        authconncount++;
    }
    else if (tochange == serverfields::connstates::UNAUTH && newstate == serverfields::connstates::DEAD) {
        conn->state = newstate;
        unauthconncount--;
    }
    else if (tochange != serverfields::connstates::UNAUTH && newstate == serverfields::connstates::DEAD) {
        conn->state = newstate;
        authconncount--;
    }
}