#include <unistd.h>
#include <sys/epoll.h>
#include <unordered_map>
#include "server/connection.h"
#include <memory>
#include "server/corestructures.h"
#include <queue>
#include "core/threadpool.h"
class server {
    private:
        int epollfd;
        int listenerfd;
        int reservefd;

        int unauthconncount;
        int authconncount;
        
        uint64_t clientsid;

        bool listenerdisabled;
        std::unordered_map<uint64_t,std::unique_ptr<connection>> connectionsmap;
        connection head;
        connection tail;
        
        int processevents(epoll_event& event);
        void handlemessage(connection * c);
        
        void markdead(connection * c);

        std::vector<uint64_t> toremove;
        std::queue<uint64_t> unauthqueue;

        //ip
        std::unordered_map<uint32_t,std::unique_ptr<essential::IP>> ipmap;
        uint64_t lastipsweeptime;

        void changestate(connection * conn,serverfields::connstates newstate);
        void reap();
        void nodelink(connection * nodep);
        void nodeunlink(connection * nodep);
        void nodeshifttail(connection * nodep);

        int getepollwaittimeout();

        void setepollout(connection * c,bool yes);
        void flushwb(connection * c);
        template<typename Payload>
        void sendmessage(connection * c,Payload& data);

        hashpool hthreadpool;
        essential::threadsafequeue<essential::hashresultentry> hashresultqueue;

        void handlesignup(connection * c,size_t start,uint32_t size,uint8_t mtype);
    public:   
        server();
        ~server();

        server(const server&) = delete; //default copy constructor is now illegal
        server& operator= (const server&) = delete; //copy assignment is illegal

        bool init();
        void run();

};

template<typename Payload>
void server::sendmessage(connection * c,Payload& data) {
    uint32_t payloadsize = sizeof data;
    size_t remaining = serverfields::CLIENTWRITEbufsizemax - c->wbwriteindex;

    if (remaining < (payloadsize + 4)) {
        markdead(c);
        return;
    }

    memcpy(c->writebuffer + c->wbwriteindex,&payloadsize,sizeof payloadsize);
    c->wbwriteindex += sizeof payloadsize;

    memcpy(c->writebuffer + c->wbwriteindex,&data,payloadsize);
    c->wbwriteindex += payloadsize;
}