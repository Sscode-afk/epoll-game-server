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
#include "core/threadpool.h"
#include "core/database.h"

extern "C" {
    #include <argon2.h>
}

//POLICY: MEMORY ALLOCATION FAILURES ARE LEFT UNHANDLED
server::server() : serverdatabase(hthreadpool) {
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

    //initialize the database
    int serverinit = serverdatabase.init();
    if (!serverinit) return false;

    head.next = &tail;
    tail.prev = &head;

    size_t hashencodedbufferlen = argon2_encodedlen(serverfields::t_cost,serverfields::m_cost,serverfields::parallelism,serverfields::saltlen,serverfields::hashlen,Argon2_id);
    SERVERASSERT(hashencodedbufferlen <= serverfields::hashresultbufsize);
    //encoded len includes the null terminator, hashing will not use the fill() method of the fixed charbuf
    //so above condition is sufficient.

    int threadsinit = hthreadpool.init(serverfields::hashpoolthreadcount,hashresultqueue,hashencodedbufferlen);
    SERVERASSERT(threadsinit);
    
    //Check if connection states follow the correct order required by the transition table
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::NONE) == 0);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::UNAUTH) == 1);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::IDLE) == 2);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::INMATCH) == 3);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::DEAD) == 4);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::LOGINDB) == 5);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::LOGINHASH) == 6);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::SIGNUPDBCHECK) == 7);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::SIGNUPHASH) == 8);
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::SIGNUPDBWRITE) == 9);

    //This invariant ensures that a connection does not spin on buffer being filled by a buggy/malicious packet
    SERVERASSERT(serverfields::MAXpacketsize + 4 <= serverfields::CLIENTREADbufsizemax);

    LOGINFO("Listenerfd (%d) and Epollfd(%d) set, server init completed.",listenerfd,epollfd);
    return true;
}

void server::run() {
    LOGINFO("Running the server...!");
    while(true) {
        epoll_event eventbuffer[serverfields::EPEVENTbufsize];

        int events;
        if ((events = epoll_wait(epollfd,eventbuffer,serverfields::EPEVENTbufsize,getepollwaittimeout())) == -1) {
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
        checkresults();
        if (stopserver) return;
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
                    newconn->rbreadindex = 0;
                    newconn->rbwriteindex = 0;
                    
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
        if (currentconn->alive == true) {
            flushwb(currentconn);
        } 
    }

    return 0;
}

void server::handlemessage(connection * c) {
    if (c->alive == false) return;

    while (true) {
        size_t remaining = serverfields::CLIENTREADbufsizemax - c->rbwriteindex;

        if (remaining == 0) {
            //the read buffer has filled up, break
            break;
        }

        ssize_t bytes = recv(c->connfd,c->readbuffer + c->rbwriteindex,remaining,0);

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
            c->rbwriteindex += bytes;
        }
    }

    //now the buffer is ready to be read
    while (true) {
        //CHANGE: A handler can now markdead a connection, guard after every message handled
        parsedata parsemsg = parse(c->readbuffer,c->rbreadindex,c->rbwriteindex);
        
        if (parsemsg.res == parseresult::INCOMPLETEMESSAGE) {
            break;
        }
        else if (parsemsg.res == parseresult::OUTOFBOUNDSMSG) {
            LOGWARNING("Incoming packet size out of bounds for '%s', dropping!",c->tag);
            markdead(c);
            return;
        }
        else {
            LOGINFO("Succesfully parsed incoming packet for '%s'!",c->tag);
        }

        switch(parsemsg.payloadtype) {
            case 1 : {
                handlesignup(c,parsemsg.payloadpointer,parsemsg.payloadsize,parsemsg.payloadtype);
                break;
            }
            default : {
                //unreachable for healthy client -- BADTYPE
                LOGWARNING("Unknown message type for %s, dropping!",c->tag);
                markdead(c);
                return;
            }
        }

        if (c->alive == false) return;

        //unauth client is not linked, a non-auth type packet will hit the unlink assertion
        //check if the node is linked first
        else if (c->next != nullptr && c->prev != nullptr){
            //No error and valid type, any msg indicates aliveness
            c->lastseen = now();
            nodeshifttail(c); 
        }
    }

    if (c->rbreadindex == c->rbwriteindex) {
        c->rbreadindex = 0;
        c->rbwriteindex = 0;
    }

    if (c->rbreadindex > 0) {
        //this branch will run if incomplete messages are present in the readbuffer.
        size_t tomove = c->rbwriteindex - c->rbreadindex;
        memmove(c->readbuffer,c->readbuffer+c->rbreadindex,tomove);
        c->rbreadindex = 0;
        c->rbwriteindex = tomove;
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
            //before removal)
            unauthqueue.pop();
        }
        else if (it->second->alive == false) {
            unauthqueue.pop(); //someone marked the client dead and they are to be removed in this
            //reap cycle!
        }
        else if (it->second->state != serverfields::connstates::UNAUTH) {
            unauthqueue.pop();
        }
        else if ((unauthnow - it->second->lastseen) >= serverfields::authtimeout) {
            markdead(it->second.get());
            unauthqueue.pop();
        }
        else break; //if an unauth client has not timed out, nobody after him has!
    }
    //ORDERING MATTERS ABOVE! LASTSEEN ACCESS IN QUEUE IS ONLY FOR UNAUTH CONNECTIONS
    //THE SAME FIELD IS USED FOR UPDATING DEADLINE OF AUTH CLIENTS!

    uint64_t authnow = now();
    while (head.next != &tail) {
        //equality ensures that epoll_wait deadline arithmetic agrees with one below
        if ((authnow - head.next->lastseen) >= serverfields::idletimeout) {
            markdead(head.next);
        }
        else break;
    }

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

bool isvalidtransition(serverfields::connstates oldstate,serverfields::connstates newstate) {
    return (serverfields::transitiontable[static_cast<size_t>(oldstate)][static_cast<size_t>(newstate)]);
}

void server::changestate(connection * conn,serverfields::connstates newstate) {
    serverfields::connstates oldstate = conn->state;
    if (!isvalidtransition(oldstate,newstate)) {
        LOGWARNING("Invalid state transition for client(%s) from state index %zu to %zu, dropping client!",conn->tag,static_cast<size_t>(oldstate),static_cast<size_t>(newstate));
        markdead(conn);
        return;
    }
    if (isunauth(oldstate) && isauth(newstate)) {
        //authentication success
        unauthconncount--;
        authconncount++;
        nodelink(conn);
    }
    else if (isauth(oldstate) && !isauth(newstate)) {
        //DEAD is the only non AUTH state that AUTH states can go to
        authconncount--;
        nodeunlink(conn);
    }
    else if (isunauth(oldstate) && !(isunauth(newstate) || isauth(newstate))) {
        unauthconncount--;
    }
    else if (!(isunauth(oldstate) || isauth(oldstate)) && isunauth(newstate)) {
        unauthconncount++;
    }

    conn->state = newstate;
}

void server::nodelink(connection * nodep) {
    SERVERASSERT((nodep->next == nullptr) && (nodep->prev == nullptr));
    nodep->next = &tail;
    nodep->prev = tail.prev;
    tail.prev->next = nodep;
    tail.prev = nodep;
}

void server::nodeunlink(connection * nodep) {
    SERVERASSERT((nodep->next != nullptr) && (nodep->prev != nullptr));
    nodep->next->prev = nodep->prev;
    nodep->prev->next = nodep->next;
    nodep->next = nullptr;
    nodep->prev = nullptr;
}

void server::nodeshifttail(connection * nodep) {
    if (nodep->next == &tail) { //node already at last
        return;
    }

    nodeunlink(nodep);
    nodelink(nodep);
}

//millisecond units
int server::getepollwaittimeout() {
    uint64_t currentime = now();
    uint64_t wakeup = UINT64_MAX;

    if (!unauthqueue.empty()) {
        uint64_t& id = unauthqueue.front();
        auto it = connectionsmap.find(id);
        if (it != connectionsmap.end() && it->second->state == serverfields::connstates::UNAUTH) {
            //lastseen for unauth clients is only set on promotion, so connections passing the check still
            //refer to the correct interval!
            uint64_t unauthdeadline = it->second->lastseen + serverfields::authtimeout;
            if (unauthdeadline < wakeup) wakeup = unauthdeadline;
        }
    }

    if (head.next != &tail) {
        uint64_t authdeadline = head.next->lastseen + serverfields::idletimeout;
        if (authdeadline < wakeup) wakeup = authdeadline;
    }

    if (wakeup == UINT64_MAX) {
        //if no clients connected block indefinitely
        return -1;
    }
    
    if (wakeup < currentime) return 0;
    return (static_cast<int>(wakeup - currentime));
}

void server::setepollout(connection * c,bool yes) {
    if (yes == c->polloutactive) return;

    epoll_event ev;
    ev.data.ptr = c;

    if (yes) {
        ev.events = EPOLLIN | EPOLLOUT;
        c->polloutactive = true;
    }
    else {
        ev.events = EPOLLIN;
        c->polloutactive = false;
    }

    if (epoll_ctl(epollfd,EPOLL_CTL_MOD,c->connfd,&ev) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"Failed setting EPOLLOUT, closing client!");
        markdead(c);
    }
}

ssize_t server::sendbytes(connection * c,size_t tosend) {
    ssize_t bytessent = send(c->connfd,c->writebuffer.getsentpointer(),tosend,MSG_NOSIGNAL);
    if (bytessent == -1) {
        int errnum = errno;
        if (errnum == EAGAIN || errnum == EWOULDBLOCK) {
            //send buffer is already full
            setepollout(c,true);
            return -1;
        }
        else {
            markdead(c);
            return -1;
        }
    }

    return bytessent;
}
void server::flushwb(connection * c) {
    SERVERASSERT(c->alive == true); //This method only operates on alive clients, it is the caller's responsibility to ensure so
    size_t contiguousslots = c->writebuffer.totalcontiguousfilledslots();
    ssize_t bytessent;
    if ((bytessent = sendbytes(c,contiguousslots)) == -1) return;

    if (static_cast<size_t>(bytessent) < contiguousslots) {
        setepollout(c,true);
        c->writebuffer.consumeslots(static_cast<size_t>(bytessent));
        return;
    }
    else if (contiguousslots == c->writebuffer.count) {
        //all bytes sent in single send call
        c->writebuffer.consumeslots(static_cast<size_t>(bytessent));
        setepollout(c,false);
        return;
    }
    
    c->writebuffer.consumeslots(contiguousslots);
    //all contiguous bytes sent, but slots to send still remain, sentpointer wraps around to 0
    contiguousslots = c->writebuffer.totalcontiguousfilledslots();
    if ((bytessent = sendbytes(c,contiguousslots)) == -1) return;
    c->writebuffer.consumeslots(static_cast<size_t>(bytessent));

    if (static_cast<size_t>(bytessent) < contiguousslots) {
        setepollout(c,true);
        return;
    }

    setepollout(c,false);
}

void server::broadcastshutdown() {
    for (auto& connpair:connectionsmap) {
        messages::servershutdownpacket msg;
        sendmessage(connpair.second.get(),msg);
    }
    stopserver = true;
}

void server::dbresultloginhandler(connection * c, dbresultentry& res) {
    if (c->state != serverfields::connstates::LOGINDB) {
        LOGWARNING("Invalid state for client(%s) (NON LOGINDB), dropping client!",c->tag);
        markdead(c);
        return;
    }
    if (res.status == dbresultstatus::SUCCESS) {
        changestate(c,serverfields::connstates::LOGINHASH);
    }
    else if (res.status == dbresultstatus::READNOTFOUND) {
        changestate(c,serverfields::connstates::UNAUTH);
        messages::serverloginresponse response(network::servresponse::LOGINFAILED);
        sendmessage(c,response);
    }    
    else {
        SERVERASSERT(0);
    }
}

void server::dbresultusercheckhandler(connection * c,dbresultentry& res) {
    if (c->state != serverfields::connstates::SIGNUPDBCHECK) {
        markdead(c);
        LOGWARNING("Invalid state for client(%s) (NON SIGNUPDBCHECK), dropping client!",c->tag);
        return;
    }
    if (res.status == dbresultstatus::SUCCESS) {
        changestate(c,serverfields::connstates::UNAUTH);
        messages::serversignupresponse response(network::servresponse::SIGNUPNAMETAKEN);
        sendmessage(c,response);
    }
    else if (res.status == dbresultstatus::READNOTFOUND) {
        changestate(c,serverfields::connstates::SIGNUPHASH);
        essential::hashentry hashjob{.connid = c->connid,.accountid = 0, .jtype = essential::hashpooljobtype::HASH, .password = c->passwordbuf};
        hthreadpool.submit(std::move(hashjob));

        //password no longer needed
        explicit_bzero(c->passwordbuf.buf,sizeof c->passwordbuf.buf);
    }
    else {
        SERVERASSERT(0);
    }
}


void server::dbresultnewuserhandler(connection * c,dbresultentry& res) {
    if (c->state != serverfields::connstates::SIGNUPDBWRITE) {
        markdead(c);
        LOGWARNING("Invalid state for client(%s) (NON SIGNUPDBWRITE), dropping client!",c->tag);
        return;
    }
    if (res.status == dbresultstatus::SUCCESS) {
        changestate(c,serverfields::connstates::IDLE);
        messages::serversignupresponse response(network::servresponse::SIGNUPSUCCESS);
        sendmessage(c,response);

        //create a session now
        //...
    }
    else if (res.status == dbresultstatus::WRITEFAILEDUNIQUE) {
        changestate(c,serverfields::connstates::UNAUTH);
        messages::serversignupresponse response(network::servresponse::SIGNUPNAMETAKEN);
        sendmessage(c,response);
    }
    else {
        SERVERASSERT(0);
    }
}

void server::checkresults() {
    std::queue<dbresultentry> dbresults;
    serverdatabase.getresults(dbresults);

    while (!dbresults.empty()) {
        dbresultentry& res = dbresults.front();
        if (res.status == dbresultstatus::FAILEDFATAL) {
            LOGINFO("Broadcasting shutdown to all connections.");
            broadcastshutdown();
            return;
        }  
        
        //discard the result if the connection object is dead
        auto it = connectionsmap.find(res.connid);
        if (it == connectionsmap.end()) {
            dbresults.pop();
            continue;
        }

        connection * c = it->second.get();
        //If a previous result in the batch marked the connection dead.
        if (c->alive == false) {
            dbresults.pop();
            continue;
        }
        //all non fatal packets have a jobtype set
        switch (res.resultjob) {
            case dbjobtype::READLOGINVERIFY : {
                dbresultloginhandler(c,res);
                break;
            }
            case dbjobtype::READCHECKIFUSEREXISTS : {
                dbresultusercheckhandler(c,res);
                break;
            }
            case dbjobtype::WRITENEWUSER : {
                dbresultnewuserhandler(c,res);
                break;
            }
            default : {
                SERVERASSERT(0);
            }
        }

        dbresults.pop();
    }

    std::queue<essential::hashresultentry> hashresults;

    //returns false if the result queue is empty
    if (!hashresultqueue.tryswap(hashresults)) return;

    while (!hashresults.empty()) {
        essential::hashresultentry& hashres  = hashresults.front();
        
        auto it = connectionsmap.find(hashres.id);
        if (it == connectionsmap.end()) {
            hashresults.pop();
            continue;
        };

        connection * c = it->second.get();

        if (c->alive == false) {
            hashresults.pop();
            continue;
        }

        if (hashres.jtype == essential::hashpooljobtype::HASH) {
            if (c->state != serverfields::connstates::SIGNUPHASH) {
                markdead(c);
                LOGWARNING("Invalid state for client(%s) (NON SIGNUPHASH), dropping client!",c->tag);
            }
            else {
                if (hashres.status == essential::hashresultstatus::ENCODEDONE) {
                    changestate(c,serverfields::connstates::SIGNUPDBWRITE);
                    serverdatabase.submitsignupjob(c->connid,c->usernamebuf,c->emailbuf,hashres.hash);
                }
                else if (hashres.status == essential::hashresultstatus::ENCODEFAILED) {
                    changestate(c,serverfields::connstates::UNAUTH);
                    
                    messages::serversignupresponse response(network::servresponse::SIGNUPFAILED);
                    sendmessage(c,response);
                }
                else {
                    SERVERASSERT(0);
                }
            }
        }
        else if (hashres.jtype == essential::hashpooljobtype::VERIFY) {
            if (c->state != serverfields::connstates::LOGINHASH) {
                markdead(c);
                LOGWARNING("Invalid state for client(%s) (NON LOGINHASH), dropping client!",c->tag);
            }
            else {
                if (hashres.status == essential::hashresultstatus::VERIFYDONE) {
                    changestate(c,serverfields::connstates::IDLE);
                    messages::serverloginresponse response(network::servresponse::LOGINSUCCESS);
                    sendmessage(c,response);

                    //session creation
                    //...
                }
                else if (hashres.status == essential::hashresultstatus::VERIFYFAIL) {
                    changestate(c,serverfields::connstates::UNAUTH);
                    messages::serverloginresponse response(network::servresponse::LOGINFAILED);
                    sendmessage(c,response);
                }
                else {
                    SERVERASSERT(0);
                }
            }

        }
    }

}

bool server::isunauth(serverfields::connstates state) {
    return (state == serverfields::connstates::UNAUTH || state == serverfields::connstates::LOGINDB || state == serverfields::connstates::LOGINHASH || state == serverfields::connstates::SIGNUPDBCHECK || state == serverfields::connstates::SIGNUPHASH || state == serverfields::connstates::SIGNUPDBWRITE);
}

bool server::isauth(serverfields::connstates state) {
    return (state == serverfields::connstates::IDLE || state == serverfields::connstates::INMATCH);
}