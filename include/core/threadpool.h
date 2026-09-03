#pragma once
#include <thread>
#include <vector>
#include "server/corestructures.h"

struct hashpool {
    private:
        std::vector<std::thread> threads;
        essential::threadsafequeue<essential::hashentry> jobqueue;
        size_t encodedlen;

        void work(essential::threadsafequeue<essential::hashresultentry>& resqueue);
    public:   
       ~hashpool();
        bool init(int n,essential::threadsafequeue<essential::hashresultentry>& rq,size_t len);
        void submit(essential::hashentry&& job);
};