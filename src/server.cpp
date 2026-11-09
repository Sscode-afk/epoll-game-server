#include "server/server.h"
#include "server/connection.h"
#include "proto/log.h"
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
#include "proto/timer.h"
#include <algorithm>
#include "core/threadpool.h"
#include "core/database.h"
#include "server/session.h"
#include <sys/eventfd.h>
#include "proto/net.h"
#include <chrono>
#include <algorithm>

extern "C" {
    #include <argon2.h>
}

//POLICY: MEMORY ALLOCATION FAILURES ARE LEFT UNHANDLED
server::server() {
    epollfd = -1;
    listenerfd = -1;
    clientsid = 1;
    listenerdisabled = false;
    authconncount = 0;
    unauthconncount = 0;

    reservefd = -1;
    resulteventfd = -1;
    udpcloseeventfd = -1;

    lastipsweeptime = now();

    listener.type = epollfdtype::LISTENER;
    wakeupevent.type = epollfdtype::RESULT;
    udpevent.type = epollfdtype::UDPCLOSE;
}

server::~server() {
    serverdatabase.stop();
    hthreadpool.stop();
    
    if (epollfd >= 0) close(epollfd);
    if (listenerfd >= 0) close(listenerfd);
    if (reservefd >= 0) close(reservefd);
    if (resulteventfd >= 0) close(resulteventfd);
    if (udpcloseeventfd >= 0) close(udpcloseeventfd);
}

bool server::init() {
    if ((reservefd = open("/dev/null",O_RDONLY)) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"fcntl reserve open() err!");
        return false;
    }
    if ((resulteventfd = eventfd(0,EFD_NONBLOCK)) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"eventfd open err!");
        return false;
    }
    if ((udpcloseeventfd = eventfd(0,EFD_NONBLOCK)) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"udpclose eventfd open err!");
        return false;
    }
    if ((listenerfd = getsockfd(socktype::TCP,network::PORT)) == -1) {
        return false;
    }

    if ((epollfd = epoll_create1(0)) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum, "epoll create err!");
        return false;
    }
    
    if (listen(listenerfd,SOMAXCONN) == -1) {
        int errnum  = errno;
        LOGERRORNUM(errnum,"listen() error on listening socket!");
        return false;
    }

    setnbresult r = setnonblocking(listenerfd);
    if (r.success == false) {
        LOGERRORNUM(r.errnum,"setnonblocking() err on listening socket");
        return false;
    }

    epoll_event listenerevent;
    listenerevent.events = EPOLLIN;
    basefd * listenerptr = &listener;
    listenerevent.data.ptr = listenerptr;

    if (epoll_ctl(epollfd,EPOLL_CTL_ADD,listenerfd,&listenerevent) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"listener epoll_ctl_add err!");
        return false;
    }

    epoll_event resultevent;
    resultevent.events = EPOLLIN;
    basefd * resulteventptr = &wakeupevent;
    resultevent.data.ptr = resulteventptr;

    if (epoll_ctl(epollfd,EPOLL_CTL_ADD,resulteventfd,&resultevent) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"resultevent epoll_ctl_add err!");
        return false;
    }

    epoll_event udpcloseevent;
    udpcloseevent.events = EPOLLIN;
    basefd * udpcloseeventptr = &udpevent;
    udpcloseevent.data.ptr = udpcloseeventptr;

    if (epoll_ctl(epollfd,EPOLL_CTL_ADD,udpcloseeventfd,&udpcloseevent) == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"udpcloseevent epoll_ctl_add err!");
        return false;
    }

    connheadsentinel.next = &conntailsentinel;
    conntailsentinel.prev = &connheadsentinel;

    sessheadsentinel.next = &sesstailsentinel;
    sesstailsentinel.prev = &sessheadsentinel;
    sessheadsentinel.dirtynext = &sesstailsentinel;
    sesstailsentinel.dirtyprev = &sessheadsentinel;

    size_t hashencodedbufferlen = argon2_encodedlen(serverfields::t_cost,serverfields::m_cost,serverfields::parallelism,serverfields::saltlen,serverfields::hashlen,Argon2_id);
    SERVERASSERT(hashencodedbufferlen <= serverfields::hashresultbufsize,"Insufficient hash buffer size");
    //encoded len includes the null terminator, hashing will not use the fill() method of the fixed charbuf
    //so above condition is sufficient.

    int threadsinit = hthreadpool.init(serverfields::hashpoolthreadcount,hashresultqueue,hashencodedbufferlen,resulteventfd);
    SERVERASSERT(threadsinit,"Hashpool init failure");
    
    //initialize the database
    int dbinit = serverdatabase.init(hashencodedbufferlen - 1,resulteventfd); //the actual hash length without the null terminator
    if (!dbinit) return false;

    //Check if connection states follow the correct order required by the transition table
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::NONE) == 0,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::UNAUTH) == 1,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::IDLE) == 2,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::INMATCH) == 3,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::DEAD) == 4,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::LOGINDB) == 5,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::LOGINHASH) == 6,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::SIGNUPDBCHECK) == 7,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::SIGNUPHASH) == 8,"Connection states order violation");
    SERVERASSERT(static_cast<size_t>(serverfields::connstates::SIGNUPDBWRITE) == 9,"Connection states order violation");

    //This invariant ensures that a connection does not spin on buffer being filled by a buggy/malicious packet
    SERVERASSERT(network::MAXpacketsize + 4 <= network::clientreadbuffersizemax,"");

    UDPthread = std::thread(&server::udpworker,this);

    LOGINFO("Listenerfd (%d) and Epollfd(%d) set, server init completed.",listenerfd,epollfd);
    return true;
}

void server::udpworker() {
    uint16_t sleeptime;
    {
        std::lock_guard<std::mutex> lck(udpworkermtx);
        sleeptime = serverfields::udpsleepduration;
    }

    std::this_thread::sleep_for(std::chrono::seconds(sleeptime));
    LOGINFO("UDPLOOP: udpworker starting execution");
    uint64_t starttime = now();

    int newfd = getsockfd(socktype::UDP,network::SERVERUDPPORTSTRING);
    if (newfd == -1) {
        seteventfd(udpcloseeventfd,"UDPLOOP",now() - starttime);
        return;
    }

    sockaddr_in clientaddr;
    socklen_t addrsize;
    while (true) {
        messages::clientudpdiscoverypacket packet;
        addrsize = sizeof clientaddr;
        ssize_t bytesreceived = recvfrom(newfd,&packet,sizeof packet,0,(sockaddr *)&clientaddr,&addrsize);
        if (bytesreceived == -1) {
            int errnum = errno;
            if (errnum == EBADF || errnum == ENOTSOCK || errnum == EINVAL) {
                //only stop for fatal errors
                LOGERRORNUM(errnum,"UDPLOOP: Closing UPD worker");
                seteventfd(udpcloseeventfd,"UDPLOOP",now() - starttime);
                return;
            }
            else continue;
        }
        else {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET,&clientaddr.sin_addr.s_addr,buf,INET_ADDRSTRLEN);
            LOGINFO("Received UDP broadcast packet from IP: %s",buf);
            
            messages::serverudpresponse responsepacket;
            ssize_t bytessent = sendto(newfd,&responsepacket,sizeof responsepacket,MSG_NOSIGNAL,(sockaddr *)&clientaddr,addrsize);
            if (bytessent == -1) {
                int errnum = errno;
                if (errnum == EBADF || errnum == ENOTSOCK || errnum == EINVAL) {
                    LOGERRORNUM(errnum,"UDPLOOP: Closing UPD worker");
                    seteventfd(udpcloseeventfd,"UDPLOOP",now() - starttime);
                    return;
                }
                else continue;
            }
        }
    }
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

        if (now() - lastsessionupdateflush >= serverfields::sessionupdateflushinterval) {
            lastsessionupdateflush = 0;
            flushdirtysessions();
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
    basefd * base = static_cast<basefd *>(event.data.ptr);

    if (base->type == epollfdtype::CONNECTION) {
        connection * c = static_cast<connection *>(base);
        if (c->alive == false) return 0;
    }
    if (event.events & (EPOLLERR | EPOLLHUP)) { 

        int socketerr = 0;
        socklen_t len = sizeof(socketerr);
        if (base->type == epollfdtype::LISTENER) {
            //to get the actual error
            if (getsockopt(listenerfd,SOL_SOCKET,SO_ERROR,&socketerr,&len) == -1) {
                int errnum = errno;
                LOGERRORNUM(errnum, "Listener error msg fetch failed!");
            }
            else if (socketerr != 0) {
                LOGERRORNUM(socketerr,"Listner error!");
            }
            return -1; //listener error, fatal
        }
        if (base->type == epollfdtype::RESULT) {
            int errnum = errno;
            LOGERRORNUM(errnum,"Resultevent error!");
            return -1;
        }

        connection * c = static_cast<connection *>(base);
        handlemessage(c); //handle any pending messages
        markdead(c);
        return 0;
    }
    if (event.events & EPOLLIN) {
        if (base->type == epollfdtype::LISTENER) {
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
                            basefd * listenerptr = &listener;
                            ev.data.ptr = listenerptr;

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
                        //keep the server alive for the healthy clients but stopping the accept loop
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
                    SERVERASSERT(unauthconncount <= serverfields::MAXunauthconnlimit, "unauth connections exceeded their cap");
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

                    setnbresult r = setnonblocking(newfd);
                    if (r.success == false) {
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
                    newconn->type = epollfdtype::CONNECTION; //Parent basefd type
                    
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
                    unauthqueue.push_back(clientsid);

                    auto [connit,conninserted] = connectionsmap.try_emplace(clientsid,std::move(newconn));
                    SERVERASSERT(conninserted == true,"Map connection insert failed, entry already exists?"); //since clientsid is incremented per client, for a new conn, the entry cannot exist
                    clientsid++;
                }
            }
            
        }
        else if (base->type == epollfdtype::UDPCLOSE && runudploop) {
            uint64_t udpthreadruntime = readeventfd(udpcloseeventfd,"UDPLOOP");
            if (udpthreadruntime >= serverfields::udpthreadruntimeok) {
                serverfields::udprunshortrestartcount = 0;
                {
                    std::lock_guard<std::mutex> lck(udpworkermtx);
                    serverfields::udpsleepduration = std::max(serverfields::udpminsleep,static_cast<uint16_t>(serverfields::udpsleepduration/2));
                }
            }
            else {
                {
                    std::lock_guard<std::mutex> lck(udpworkermtx);
                    serverfields::udpsleepduration = std::min(static_cast<uint16_t>(serverfields::udpsleepduration * 2),serverfields::udpmaxsleep);

                }
                serverfields::udprunshortrestartcount++;

                if (serverfields::udprunshortrestartcount >= serverfields::maxudploopretries) {
                    runudploop = false;
                }
            }

            if (UDPthread.joinable()) {
                UDPthread.join();
            }

            if (runudploop) UDPthread = std::thread(&server::udpworker,this);
        }

        else if (base->type == epollfdtype::RESULT) {
            //eventfd was set, it's only job was to signal the epoll loop to wake up.
            //we reset the counter to 0 so that future signals can wake the loop again
            //read failure above is fatal in all cases
            //the eventfd counter is not consumed by any thread, process etc. between epoll_wait
            //and reading, this means EAGAIN or EWOULDBLOCK are a violation of this invariant
            //since event fd is set with the flag EFD_NONBLOCK, the read cannot be interrupted
            readeventfd(resulteventfd,"SERVER");
        }
        else {
            connection * c = static_cast<connection *>(base);
            handlemessage(c);
        }
    }
    if (event.events & EPOLLOUT) {
        connection * c = static_cast<connection *>(base);
        if (static_cast<connection *>(base)->alive == true) {
            flushresult fres = flushbytes(c->connfd,c->writebuffer);
            switch (fres.status) {
                case flushstatus::ERROR : {
                    LOGWARNINGNUM(fres.errcode,"Socket error while writing on client(%s), dropping client!",c->tag);
                    markdead(c);
                    break;
                }
                case flushstatus::FULLFLUSH : {
                    setepollout(c,false);
                    break;
                }
                case flushstatus::PARTIALFLUSH :
                case flushstatus::SOSENDBUFFERFULL : {
                    setepollout(c,true);
                    break;
                }
                default : {
                    SERVERFAIL("Unknown enum status returned by fillwritebuffer(...)");
                    break;
                }
            }
        } 
    }

    return 0;
}

void server::handlemessage(connection * c) {
    if (c->alive == false) return;
    readresult r = fillreadbuffer(c->connfd,c->readbuffer);

    switch(r.status) {
        case readstatus::ERROR : {
            LOGWARNINGNUM(r.errcode,"recv() failure on client (%s)!",c->tag);
            markdead(c);
            return; 
        }
        case readstatus::SOCKETFIN : {
            LOGINFO("client(%s) closed their connection (FIN)!",c->tag);
            markdead(c);
            return;
        }
        case readstatus::READBUFFERFULL :
        case readstatus::DONE : {
            //parse
            break;
        }
        default : {
            SERVERFAIL("Unknown readstatus enum value!");
            return;
        }
    }
    
    //now the buffer is ready to be read
    while (true) {
        //CHANGE: A handler can now markdead a connection, guard after every message handled
        parsedata parsemsg = parse(c->readbuffer);
        
        if (parsemsg.res == parseresult::INCOMPLETEMESSAGE || parsemsg.res == parseresult::ALLREAD) {
            break;
        }
        else if (parsemsg.res == parseresult::OUTOFBOUNDSMSG) {
            LOGWARNING("Incoming packet size out of bounds for '%s', dropping!",c->tag);
            markdead(c);
            return;
        }
        else if (parsemsg.res == parseresult::SUCCESS) {
            LOGINFO("Succesfully parsed incoming packet for '%s'!",c->tag);
        }
        else {
            SERVERFAIL("Unknown parseresult enum value!");
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
            connectionnodeshifttail(c); 
        }
    }

    c->readbuffer.compact();

}
//MARKDEAD NEVER DELETES, just sets alive to false
//Deletion only happens in toreap(). any method during processevents always gets a valid raw pointer
void server::markdead(connection * c) {
    if (c->alive == false) return; //some previous event in the batch already marked this client dead
    
    if (isauth(c->state)) {
        //an auth, alive connection always has a valid session
        Session * s = getsession(c);
        s->activeconnectionid = 0;
        s->sessionactive = false;
        s->lastseen = now();
        
        sessionnodelink(s);
    }

    c->alive = false;
    //decrement IP count
    auto pit = ipmap.find(c->connaddr.sin_addr.s_addr);

    //IP counts are only decremented in markdead, and incremented on accept
    //markdead called on a connection MUST have its corresponding ipmap entry alive as non zero count does not reap it
    SERVERASSERT(pit!=ipmap.end(),"Live connection has no IP map entry keyed by its addr field"); 
    pit->second->count -= 1;
    //evaluate tokens before checking

    essential::updatetokens(pit->second->tb,now());
    if (pit->second->count == 0 && pit->second->tb.tokens == pit->second->tb.capacity) ipmap.erase(pit); //oppurtunistic early cleanup
    toremove.push_back(c->connid);
    
    changestate(c,serverfields::connstates::DEAD);

    LOGINFO("Client(%s) marked dead!",c->tag);
}

void server::deletesession(Session * s) {
    if (s->dirty == true) {
        //if the connection is inactive, then activeconnectionid i
        serverdatabase.submitupdateusercachejob(s);
        s->dirty = false;
        sessionnodeunlink(s,true); //unlink after pushing the update job
        return;
    }

    SERVERASSERT(tokenmap.erase(s->token),"Token entry corresponding to session being deleted does not exist");
    SERVERASSERT(sessionmap.erase(s->accountid),"Deletesession handed an already dead/removed session");
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
            unauthqueue.pop_front();
        }
        else if (it->second->alive == false) {
            unauthqueue.pop_front(); //someone marked the client dead and they are to be removed in this
            //reap cycle!
        }
        else if (!isunauth(it->second->state)) {
            unauthqueue.pop_front();
        }
        else if ((unauthnow - it->second->lastseen) >= serverfields::authtimeout) {
            markdead(it->second.get());
            unauthqueue.pop_front();
        }
        else break; //if an unauth client has not timed out, nobody after him has!
    }
    //ORDERING MATTERS ABOVE! LASTSEEN ACCESS IN QUEUE IS ONLY FOR UNAUTH CONNECTIONS
    //THE SAME FIELD IS USED FOR UPDATING DEADLINE OF AUTH CLIENTS!

    uint64_t authnow = now();
    while (connheadsentinel.next != &conntailsentinel) {
        //equality ensures that epoll_wait deadline arithmetic agrees with one below
        if ((authnow - connheadsentinel.next->lastseen) >= serverfields::idletimeout) {
            markdead(connheadsentinel.next);
        }
        else break;
    }

    while (sessheadsentinel.next != &sesstailsentinel) {
        //In the session expiry linked list, only an inactive session must be present
        Session * s = sessheadsentinel.next;
        SERVERASSERT(s->sessionactive == false,"Active session found in session expiry list");
        if ((authnow - s->lastseen) >= serverfields::sessiontimeout) {
            //connections can be deemed 'dead' at any point within the epoll batch, but sessions can
            //always be used and have a single point of expiry, so they do not have a markdead() equivalent
            //and can be directly removed here post epoll batch if the conditions are met.

            sessionnodeunlink(s);

            if (s->outboundasyncjobs == 0) {
                //no jobs in flight, no need to drain
                //both final session expiry and token expiry take place at a single point
                deletesession(s);
            }
            else {
                //the session has unlinked from the expiry list, but still remains in the map
                //waiting for all outbound jobs to complete
                //This is the ONLY point where this flag is set true. any takeover or reconnect sets it back to false
                s->clearingoutboundjobs = true;
            }
        }
        else break;
    }

    //IF connections will be cleared and listener is disabled
    if (!toremove.empty() && listenerdisabled) {
        epoll_event ev;
        basefd * listenerptr = &listener;
        ev.data.ptr = listenerptr;
        ev.events = EPOLLIN;
        int enablelistener = epoll_ctl(epollfd,EPOLL_CTL_MOD,listenerfd,&ev);

        SERVERASSERT(enablelistener!=-1,"Listener re-enable failed");
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
    SERVERASSERT(static_cast<size_t>(authconncount + unauthconncount) == connectionsmap.size(),"");

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

    //Invalid transitions are a result of a failure in proper state management handling by the server code
    SERVERASSERT(isvalidtransition(oldstate,newstate),"Invalid state transition from %d to %d on Client(%s)",static_cast<int>(oldstate),static_cast<int>(newstate),conn->tag);

    if (isunauth(oldstate) && isauth(newstate)) {
        //authentication success
        unauthconncount--;
        authconncount++;
        connectionnodelink(conn);
    }
    else if (isauth(oldstate) && !isauth(newstate)) {
        //DEAD is the only non AUTH state that AUTH states can go to
        authconncount--;
        connectionnodeunlink(conn);
    }
    else if (isunauth(oldstate) && !(isunauth(newstate) || isauth(newstate))) {
        unauthconncount--;
    }
    else if (!(isunauth(oldstate) || isauth(oldstate)) && isunauth(newstate)) {
        unauthconncount++;
    }

    conn->state = newstate;
}

void server::connectionnodelink(connection * nodep) {
    SERVERASSERT((nodep->next == nullptr) && (nodep->prev == nullptr),"connection node being linked is not fully unlinked");
    nodep->next = &conntailsentinel;
    nodep->prev = conntailsentinel.prev;
    conntailsentinel.prev->next = nodep;
    conntailsentinel.prev = nodep;
}

void server::connectionnodeunlink(connection * nodep) {
    SERVERASSERT((nodep->next != nullptr) && (nodep->prev != nullptr),"connection node being unlinked is not fully linked");
    nodep->next->prev = nodep->prev;
    nodep->prev->next = nodep->next;
    nodep->next = nullptr;
    nodep->prev = nullptr;
}

void server::connectionnodeshifttail(connection * nodep) {
    if (nodep->next == &conntailsentinel) { //node already at last
        return;
    }

    connectionnodeunlink(nodep);
    connectionnodelink(nodep);
}

void server::sessionnodelink(Session * nodep,bool dirtylink) {
    if (dirtylink) {
        SERVERASSERT((nodep->dirtynext == nullptr) && (nodep->dirtyprev == nullptr),"session node being linked to dirty list is not fully unlinked");
        nodep->dirtynext = &sesstailsentinel;
        nodep->dirtyprev = sesstailsentinel.dirtyprev;
        sesstailsentinel.dirtyprev->dirtynext = nodep;
        sesstailsentinel.dirtyprev = nodep;

        dirtysessionslistsize++;
        return;
    }
    
    SERVERASSERT((nodep->next == nullptr) && (nodep->prev == nullptr),"session node being linked to expiry list is not fully unlinked");
    nodep->next = &sesstailsentinel;
    nodep->prev = sesstailsentinel.prev;
    sesstailsentinel.prev->next = nodep;
    sesstailsentinel.prev = nodep;
}

void server::sessionnodeunlink(Session * nodep,bool dirtyunlink) {
    if (dirtyunlink) {
        SERVERASSERT((nodep->dirtynext != nullptr) && (nodep->dirtyprev != nullptr),"session node being unlinked from dirty list is not fully linked");
        nodep->dirtyprev->dirtynext = nodep->dirtynext;
        nodep->dirtynext->dirtyprev = nodep->dirtyprev;
        nodep->dirtynext = nullptr;
        nodep->dirtyprev = nullptr;

        dirtysessionslistsize--;
        return;
    }

    SERVERASSERT((nodep->next != nullptr) && (nodep->prev != nullptr),"session node being unlinked from expiry list is not fully linked");
    nodep->prev->next = nodep->next;
    nodep->next->prev = nodep->prev;
    nodep->next = nullptr;
    nodep->prev = nullptr;
}

//millisecond units
int server::getepollwaittimeout() {
    uint64_t currentime = now();
    uint64_t wakeup = UINT64_MAX;

    for (auto& id:unauthqueue) {
        auto it = connectionsmap.find(id);
        if (it != connectionsmap.end() && isunauth(it->second->state)) {
            uint64_t unauthdeadline = it->second->lastseen + serverfields::authtimeout;
            if (unauthdeadline < wakeup) wakeup = unauthdeadline;
            break;
        }
    }

    if (connheadsentinel.next != &conntailsentinel) {
        uint64_t authdeadline = connheadsentinel.next->lastseen + serverfields::idletimeout;
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

void server::broadcastshutdown() {
    for (auto& connpair:connectionsmap) {
        messages::servershutdownpacket msg;
        sendmessage(connpair.second.get(),msg);
    }
    stopserver = true;
}

//Invariant: An alive connections state always matches the expected intermediate state
//Any connection can only have a single pre auth job in flight at a time and in such intermediate states, other packets are ignored
//Dead connections never get to reach the handlers and their results are popped and skipped
void server::dbresultloginhandler(connection * c, dbresultentry& res) {
    SERVERASSERT(c->state == serverfields::connstates::LOGINDB,"");
    if (res.status == dbresultstatus::SUCCESS) {
        resultverifyparams * p = std::get_if<resultverifyparams>(&res.params);
        SERVERASSERT(p != nullptr,"Variant of login read async job not properly set to the right params struct (resultverifyparams)");
        changestate(c,serverfields::connstates::LOGINHASH);
        hthreadpool.submithashverificationjob(c->connid,res.accountid,c->passwordbuf,p->hash);

        explicit_bzero(c->passwordbuf.buf,sizeof c->passwordbuf.buf);
    }
    else if (res.status == dbresultstatus::READNOTFOUND) {
        changestate(c,serverfields::connstates::UNAUTH);
        messages::serverloginresponse response(network::servresponse::LOGINFAILED);
        sendmessage(c,response);
    }    
    else {
        SERVERFAIL("Unknown result status for async database job (login read)");
    }
}

void server::dbresultusercheckhandler(connection * c,dbresultentry& res) {
    SERVERASSERT(c->state == serverfields::connstates::SIGNUPDBCHECK,"");
    if (res.status == dbresultstatus::SUCCESS) {
        changestate(c,serverfields::connstates::UNAUTH);
        messages::serversignupresponse response(network::servresponse::SIGNUPNAMETAKEN);
        sendmessage(c,response);
    }
    else if (res.status == dbresultstatus::READNOTFOUND) {
        changestate(c,serverfields::connstates::SIGNUPHASH);    
        hthreadpool.submithashcreationjob(c->connid,c->passwordbuf);
        //password no longer needed
        explicit_bzero(c->passwordbuf.buf,sizeof c->passwordbuf.buf);
    }
    else {
        SERVERFAIL("Unknown result status for async database job (Signup read)");
    }
}


void server::dbresultnewuserhandler(connection * c,dbresultentry& res) {
    SERVERASSERT(c->state == serverfields::connstates::SIGNUPDBWRITE,"");
    if (res.status == dbresultstatus::SUCCESS) {
        //if the write succeeded, account id should be non-zero
        SERVERASSERT(res.accountid != 0,"Database new user write did not set the account id of the entered row");
        attachsession(c,serverfields::sessioncreationstage::FROMSIGNUP,res.accountid);
    }
    else if (res.status == dbresultstatus::WRITEFAILEDUNIQUE) {
        changestate(c,serverfields::connstates::UNAUTH);
        messages::serversignupresponse response(network::servresponse::SIGNUPNAMETAKEN);
        sendmessage(c,response);
    }
    else {
        SERVERFAIL("Unknown result status for async database job (Signup write)");
    }
}

void server::dbresultgetusercachehandler(connection * c, Session * s,dbresultentry& res) {
    //POST AUTH JOB, reaching here means that the session is alive and the issuing connection may or may not be alive
    //mutate the session first
    resultgetcacheparams * respointer = std::get_if<resultgetcacheparams>(&res.params);
    SERVERASSERT(respointer != nullptr,"Variant of session cache read async job not properly set to the right params struct (resultgetcacheparams)");

    s->cache.username = respointer->username;
    s->cache.email = respointer->email;
    s->cache.playeremblems = respointer->emblems;

    s->cachefilled = true;

    if (s->sessionactive) {
        if (c == nullptr) {
            //the issuing connection has been replaced by another connection
            //since the session is active, the new connection must be alive
            SERVERASSERT((c = getaliveconnptr(s->activeconnectionid)) != nullptr, "Session active despite being attached to a dead connection");
        }
        SERVERASSERT(c->state == serverfields::connstates::SESSIONCACHEGET,"");
        changestate(c,serverfields::connstates::IDLE);
        sendlobbyinfo(c,s->cache.username,s->cache.email,s->cache.playeremblems);
    }
}


connection * server::getaliveconnptr(uint64_t connid) {
    auto it = connectionsmap.find(connid);
    if (it == connectionsmap.end() || it->second->alive == false) return nullptr;
    return it->second.get();
}

bool issessionboundjob(dbjobtype type) {
    return (type == dbjobtype::WRITEUSERCACHE || type == dbjobtype::READUSERCACHE);
}

essential::dbdispatcherparams server::analyzedbresulttype(dbresultentry& res) {
    essential::dbdispatcherparams p;
    //check if issuing connection is alive
    connection * c = getaliveconnptr(res.connid);
    bool resultissessionbound;
    if ((resultissessionbound = issessionboundjob(res.resultjob)) == true) p.type = essential::dbdispatcherresult::SESSIONBOUNDJOB;
    else p.type = essential::dbdispatcherresult::NONSESSIONBOUNDJOB;

    if (c == nullptr) {
        p.connptr = nullptr;
        //issuing connection is dead
        if (resultissessionbound) {
            auto sessionit = sessionmap.find(res.accountid);
            if (sessionit == sessionmap.end()) p.sessptr = nullptr;
            else p.sessptr = sessionit->second.get();
        }
        else {
            p.sessptr = nullptr;
        }
    }
    else {
        p.connptr = c;
        if (resultissessionbound) {
            //a POST auth job is linked to an auth connection
            p.sessptr = getsession(p.connptr);
        }
        else {
            p.sessptr = nullptr;
        }
    }
    return p;
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
        
        essential::dbdispatcherparams p = analyzedbresulttype(res);
        //No session can die while a session bound job was out
        if (p.type == essential::dbdispatcherresult::SESSIONBOUNDJOB) {
            SERVERASSERT(p.sessptr != nullptr, "Session died while a SESSION BOUND job's result had still not been processed");
        }
        else if (p.type == essential::dbdispatcherresult::NONSESSIONBOUNDJOB) {
            if (p.connptr == nullptr) {
                dbresults.pop();
                continue;
            }
        }
        else {
            SERVERFAIL("Unknown dispatcher enum value for database result type");
        }

        //all non fatal packets have a jobtype set
        switch (res.resultjob) {
            case dbjobtype::READLOGINVERIFY : {
                dbresultloginhandler(p.connptr,res);
                break;
            }
            case dbjobtype::READCHECKIFUSEREXISTS : {
                dbresultusercheckhandler(p.connptr,res);
                break;
            }
            case dbjobtype::WRITENEWUSER : {
                dbresultnewuserhandler(p.connptr,res);
                break;
            }
            case dbjobtype::READUSERCACHE : {
                dbresultgetusercachehandler(p.connptr,p.sessptr,res);
                break;
            }
            case dbjobtype::WRITEUSERCACHE : {
                break;
            }
            default : {
                SERVERFAIL("Unknown job type for database async operation result");
            }
        }

        if (p.type == essential::dbdispatcherresult::SESSIONBOUNDJOB) {
            SERVERASSERT(p.sessptr != nullptr, "DB result analyzer provided null session pointer while session bound async operation was still out.");
            sessionpostauthtrydelete(p.sessptr);
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
                    SERVERFAIL("Unknown job result status for hashpool async operation result");
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
                    //session creation
                    attachsession(c,serverfields::sessioncreationstage::FROMLOGIN,hashres.accountid);
                }
                else if (hashres.status == essential::hashresultstatus::VERIFYFAIL) {
                    changestate(c,serverfields::connstates::UNAUTH);
                    messages::serverloginresponse response(network::servresponse::LOGINFAILED);
                    sendmessage(c,response);
                }
                else {
                    SERVERFAIL("Unknown job result status for hashpool async operation result");
                }
            }

        }
        else {
            SERVERFAIL("Unknown job type for hashpool async operation result");
        }

        hashresults.pop();
    }

}

bool server::isunauth(serverfields::connstates state) {
    return (state == serverfields::connstates::UNAUTH || state == serverfields::connstates::LOGINDB || state == serverfields::connstates::LOGINHASH || state == serverfields::connstates::SIGNUPDBCHECK || state == serverfields::connstates::SIGNUPHASH || state == serverfields::connstates::SIGNUPDBWRITE);
}

bool server::isauth(serverfields::connstates state) {
    return (state == serverfields::connstates::IDLE || state == serverfields::connstates::INMATCH);
}

void server::attachsession(connection * c,serverfields::sessioncreationstage stage,uint64_t accid) {   
    SERVERASSERT(c->alive == true,"attachsession got a dead connection");
    sessiontokenpair tokens;

    if (stage != serverfields::sessioncreationstage::FROMTOKEN) {
        while (true) {
            std::optional<sessiontokenpair> p = gettokens();
            SERVERASSERT(p != std::nullopt, "Random token generation failed");

            tokens = p.value();
            if (!tokenmap.count(tokens.hashedtoken)) break;
        }
    }

    if (stage == serverfields::sessioncreationstage::FROMLOGIN || stage == serverfields::sessioncreationstage::FROMTOKEN) {
        auto it = sessionmap.find(accid);
        if (it != sessionmap.end()) {
            //session already exists
            Session * s = it->second.get();
            //FROMTOKEN call must ensure that the reconnecting clients token is already in the tokenmap
            //If so, this method's logic ensures that the reconnecting client was not takenover by any other client
            //By the time it returned. Thus from the time the reconnecting client was markded dead to the time they try
            //to reconnect, the session was inactive.
            if (stage == serverfields::sessioncreationstage::FROMTOKEN) SERVERASSERT(s->sessionactive == false, "Reconnecting client token still in tokenmap, yet client was taken over");

            if (s->sessionactive) {
                uint64_t oldconnid = s->activeconnectionid;
                auto old = connectionsmap.find(oldconnid);

                //an active connection always has an alive connection
                SERVERASSERT(old != connectionsmap.end() && old->second->alive == true,"Active session has a dead connection attached to it");
                messages::servernewconntakeoverpacket packet;
                sendmessage(old->second.get(),packet);
                markdead(old->second.get());

                //delete the original hashed token from the tokenmap
                SERVERASSERT(tokenmap.erase(s->token), "Alive session does not have a corresponding token in tokenmap"); //the session token MUST exist in the map
            }

            s->activeconnectionid = c->connid;
            s->sessionactive = true;
            s->clearingoutboundjobs = false;

            //regardless of session being active or inactive, at this point it will be in the expiry list
            //If active, the original connection getting marked dead puts it in the list
            //If inactive, it must already be in the expiry list
            sessionnodeunlink(s);
            c->sess = s;

            if (stage == serverfields::sessioncreationstage::FROMLOGIN) {
                tokenmap.emplace(tokens.hashedtoken,accid);
                s->token = tokens.hashedtoken;

                messages::serversessiontokenpacket tokpacket;
                memcpy(tokpacket.token,tokens.rawtoken.bytes.data(),tokens.rawtoken.bytes.size());
                sendmessage(c,tokpacket);
            }

            if (s->cachefilled) {
                changestate(c,serverfields::connstates::IDLE);
                sendlobbyinfo(c,s->cache.username,s->cache.email,s->cache.playeremblems);
            }
            else {
                //If the original session fetch async job is still in flight, then upon completion
                //It will send the lobby information to the new connection instead
                changestate(c,serverfields::connstates::SESSIONCACHEGET);
            }
            return;
        }
    }
    
    std::unique_ptr<Session> newsess = std::make_unique<Session>(c->connid,accid,now(),tokens.hashedtoken);
    newsess->sessionactive = true;

    changestate(c,serverfields::connstates::SESSIONCACHEGET);
    serverdatabase.submitgetsessioncachejob(newsess.get());

    auto [sessionit,sessioninserted] = sessionmap.try_emplace(accid,std::move(newsess));
    SERVERASSERT(sessioninserted == true,"");
    
    c->sess = sessionit->second.get();
    tokenmap.emplace(tokens.hashedtoken,accid);

    messages::serversessiontokenpacket packet;
    memcpy(packet.token,tokens.rawtoken.bytes.data(),tokens.rawtoken.bytes.size());
    sendmessage(c,packet);
}

//THE INVARIANT: ONLY LIVE AUTHENTICATED CONNECTIONS MAY TRY TO FETCH A SESSION VIA A CONNECTION *
//SUCH CONNECTIONS ALWAYS HAVE A VALID NON NULL SESSION * ASSOCIATED WITH THEM WHOSE ACTIVE CONNECTION ID IS THE CONNECTIONS'S ITSELF
Session * server::getsession(connection * c) {
    SERVERASSERT(c->alive == true,"Trying to fetch a session from a dead connection");
    SERVERASSERT(c->sess != nullptr,"Trying to fetch a session from a connection not linked to one (nullptr)");
    SERVERASSERT(isauth(c->state), "Trying to fetch a connection from an UNAUTH connection");
    SERVERASSERT(c->sess->activeconnectionid == c->connid, "Trying to fetch a session from a connection which is not the session's active connection");
    return c->sess;
}

void server::sendlobbyinfo(connection * c, fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxemailsize + 1>& email, uint64_t emblems) {
    messages::serverlobbyinfo lobbyinfopacket;
    memcpy(lobbyinfopacket.username,username.buf,username.length);
    memcpy(lobbyinfopacket.email,email.buf,email.length);

    emblems = htobe64(emblems);
    lobbyinfopacket.emblems = emblems;
    sendmessage(c,lobbyinfopacket);
}

//runs whenever a post auth job (session associated) result arrives
void server::sessionpostauthtrydelete(Session * s) {
    SERVERASSERT(s != nullptr, "");
    SERVERASSERT(s->outboundasyncjobs > 0, "Try delete in response to post auth session bound result already finds no outbound session bound async jobs");
    s->outboundasyncjobs--;
    if (s->clearingoutboundjobs && s->outboundasyncjobs == 0) {
        deletesession(s);
    }
}

void server::mutatesessioncache(Session * s, uint64_t emblemsnew) {
    SERVERASSERT(s!=nullptr, "NULL session pointer handed to mutate method");
    SERVERASSERT(s->sessionactive != false, "Inactive session handed to mutate method");

    s->cache.playeremblems = emblemsnew;
    if (!s->dirty) {
        sessionnodelink(s,true);
        s->dirty = true;
    }
}

void server::flushdirtysessions() {
    while (sessheadsentinel.dirtynext != &sesstailsentinel) {
        Session * s = sessheadsentinel.dirtynext;
        sessioncacheupdatejobbuffer.push(serverdatabase.prepareusercacheupdatejob(s));
        //preparation increments the outbound async job count
        s->dirty = false;
        sessionnodeunlink(s,true);
    }
    serverdatabase.submitusercacheupdatebatch(sessioncacheupdatejobbuffer);
}