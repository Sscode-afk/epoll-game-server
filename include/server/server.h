#include <unistd.h>
#include <sys/epoll.h>
#include <unordered_map>
#include "server/connection.h"
#include <memory>
#include "server/corestructures.h"
#include <queue>
#include <deque>
#include "core/threadpool.h"
#include "core/database.h"

#include "server/session.h"
#include "proto/net.h"
#include "core/log.h"

class server {
    private:
        int epollfd;
        int listenerfd;
        int reservefd;
        int resulteventfd;

        int unauthconncount;
        int authconncount;

        basefd listener;
        basefd wakeupevent;
        
        uint64_t clientsid;

        bool listenerdisabled;
        std::unordered_map<uint64_t,std::unique_ptr<connection>> connectionsmap;
        connection connheadsentinel;
        connection conntailsentinel;
        
        int processevents(epoll_event& event);

        template <typename T>
        void sendmessage(connection * c, T& payload);
        void handlemessage(connection * c);
        
        void markdead(connection * c);

        std::vector<uint64_t> toremove;
        std::deque<uint64_t> unauthqueue;

        //ip
        std::unordered_map<uint32_t,std::unique_ptr<essential::IP>> ipmap;
        uint64_t lastipsweeptime;

        //session
        std::unordered_map<uint64_t,std::unique_ptr<Session>> sessionmap;
        Session sessheadsentinel;
        Session sesstailsentinel;
        std::unordered_map<sessiontoken,uint64_t,tokenhasher> tokenmap; 
        void attachsession(connection * c,serverfields::sessioncreationstage stage,uint64_t accid);

        void changestate(connection * conn,serverfields::connstates newstate);
        void reap();

        void connectionnodelink(connection * nodep);
        void connectionnodeunlink(connection * nodep);
        void connectionnodeshifttail(connection * nodep);

        void sessionnodelink(Session * nodep,bool dirtylink = false);
        void sessionnodeunlink(Session * nodep,bool dirtyunlink = false);

        int getepollwaittimeout();

        void setepollout(connection * c,bool yes);

        void sendlobbyinfo(connection * c, fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxemailsize + 1>& email, uint64_t emblems);

        hashpool hthreadpool;
        essential::threadsafequeue<essential::hashresultentry> hashresultqueue;

        database serverdatabase;
        void handlesignup(connection * c,size_t start,uint32_t size,uint8_t mtype);
        void handlelogin(connection * c,size_t start,uint32_t size,uint8_t mtype);

        void checkresults();
        void broadcastshutdown();
        bool stopserver = false;

        void dbresultloginhandler(connection * c,dbresultentry& res);
        void dbresultusercheckhandler(connection * c,dbresultentry& res);
        void dbresultnewuserhandler(connection * c,dbresultentry& res);
        void dbresultgetusercachehandler(connection * c,Session * s,dbresultentry& res);

        connection * getaliveconnptr(uint64_t connid);
        essential::dbdispatcherparams analyzedbresulttype(dbresultentry& res);

        Session * getsession(connection * c);
        void deletesession(Session * s);
        void sessionpostauthtrydelete(Session * s);

        size_t dirtysessionslistsize = 0;
        std::queue<dbwriteentry> sessioncacheupdatejobbuffer;
        void mutatesessioncache(Session *,uint64_t emblemsnew);
        void flushdirtysessions();

        uint64_t lastsessionupdateflush = 0;
    public: 
        server();
        ~server();

        server(const server&) = delete; //default copy constructor is now illegal
        server& operator= (const server&) = delete; //copy assignment is illegal

        bool isunauth(serverfields::connstates state);
        bool isauth(serverfields::connstates state);
        bool init();
        void run();

};

template <typename T>
void server::sendmessage(connection * c,T& payload) {
    fillmessageresult sendres = fillwritebuffer(payload,c->writebuffer,c->connfd);
    switch (sendres.status) {
        case fillmessagestatus::ERROR : {
            LOGWARNINGNUM(sendres.errcode,"Socket error while writing on client(%s), dropping client!",c->tag);
            markdead(c);
            return;
        }
        case fillmessagestatus::APPLICATIONBUFFERFULL : {
            LOGWARNING("No space left in writebuffer for client(%s), dropping client!",c->tag);
            markdead(c);
            return;
        }
        case fillmessagestatus::PARTIALFLUSH :
        case fillmessagestatus::SOCKETBUFFERFULL : {
            setepollout(c,true);
            return;
        }
        case fillmessagestatus::FULLFLUSH : {
            setepollout(c,false);
            return;
        }
        case fillmessagestatus::INVALID : {
            SERVERFAIL("Unknown enum status returned by fillwritebuffer(...)");
            return;
        }
    }
}