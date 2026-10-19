#pragma once
#include <thread>
#include <vector>
#include "server/corestructures.h"

struct hashpool {
    private:
        std::vector<std::thread> threads;
        essential::threadsafequeue<essential::hashentry> jobqueue;

        void work(essential::threadsafequeue<essential::hashresultentry>& resqueue);
        int resulteventfd = -1;
    public: 
        size_t encodedlen;  
       ~hashpool();
        bool init(int n,essential::threadsafequeue<essential::hashresultentry>& rq,size_t len, int reseventfd);

        void submithashcreationjob(uint64_t connid,fixedcharbuffer<network::maxcredentialsize + 1>& password);
        void submithashverificationjob(uint64_t connid,uint64_t accid,fixedcharbuffer<network::maxcredentialsize + 1>& password,fixedcharbuffer<serverfields::hashresultbufsize>& hash);

        void stop();
};