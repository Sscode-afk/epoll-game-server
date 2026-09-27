#pragma once
#include <thread>
#include <vector>
#include "server/corestructures.h"

struct hashpool {
    private:
        std::vector<std::thread> threads;
        essential::threadsafequeue<essential::hashentry> jobqueue;

        void work(essential::threadsafequeue<essential::hashresultentry>& resqueue);
    public: 
        size_t encodedlen;  
       ~hashpool();
        bool init(int n,essential::threadsafequeue<essential::hashresultentry>& rq,size_t len);
        void submit(essential::hashentry&& job);
};