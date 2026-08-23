#include <unistd.h>
#include <sys/epoll.h>
#include <unordered_map>
#include "server/connection.h"
#include <memory>
#include "server/corestructures.h"
#include <queue>
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
        
        int processevents(epoll_event& event);
        void handlemessage(connection * c);
        
        void markdead(connection * c);

        std::vector<uint64_t> toremove;
        std::queue<uint64_t> unauthqueue;

        //ip
        std::unordered_map<uint32_t,std::unique_ptr<essential::IP>> ipmap;
        uint64_t lastipsweeptime;

        void changestate(serverfields::connstates& tochange,serverfields::connstates newstate);
        void reap();

    public:   
        server();
        ~server();

        server(const server&) = delete; //default copy constructor is now illegal
        server& operator= (const server&) = delete; //copy assignment is illegal

        bool init();
        void run();

};