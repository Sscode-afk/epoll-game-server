#pragma once
#include "server/serversettings.h"
#include "proto/settings.h"
#include "proto/objects.h"
#include <algorithm>
#include <queue>
#include <mutex>
#include <optional>
#include <mutex>
#include <cstring>
#include <condition_variable>
#include <variant>

namespace essential {
    struct tokenbucket {
        uint64_t lastseen;
        double tokens;
        double capacity;
        double refillrate;
    };

    struct IP {
        size_t count = 0;
        tokenbucket tb = {0,serverfields::iptbcap,serverfields::iptbcap,serverfields::iptbrefillrate};

        IP(uint64_t createdat) {
            tb.lastseen = createdat;
        }
    };

    inline void updatetokens(tokenbucket& tb,uint64_t now) {
        tb.tokens = std::clamp(tb.tokens + (now - tb.lastseen) * tb.refillrate,0.0,tb.capacity);
        tb.lastseen = now;
    }

    enum class hashpooljobtype {HASH,VERIFY};
    enum class hashresultstatus {VERIFYDONE,VERIFYFAIL,ENCODEFAILED,ENCODEDONE};

    struct hashentry {
        uint64_t connid;
        uint64_t accountid = 0;
        hashpooljobtype jtype;

        fixedcharbuffer<network::maxcredentialsize + 1> password;
        fixedcharbuffer<serverfields::hashresultbufsize> hash;
    };

    struct hashresultentry {
        uint64_t id;
        uint64_t accountid = 0;
        hashpooljobtype jtype;
        hashresultstatus status;
        fixedcharbuffer<serverfields::hashresultbufsize> hash;
    };

    template <typename T>
    struct threadsafequeue {
        private:
            std::mutex mtx;
            std::condition_variable cv;
            std::queue<T> tsqueue;
            bool killthreads = false;
        public:
            void push(T&& item) {
                std::unique_lock<std::mutex> lck(mtx);
                tsqueue.push(std::move(item));
                cv.notify_one();
            }
            std::optional<T> popblock() { //for hashjobs
                std::unique_lock<std::mutex> lck(mtx);
                cv.wait(lck,[&]() -> bool {return !tsqueue.empty() || killthreads;});
                if (killthreads) return std::nullopt;
                T front = std::move(tsqueue.front());
                tsqueue.pop();
                return front;
            }
            bool tryswap(std::queue<T>& tempswapqueue) {
                std::lock_guard<std::mutex> lck(mtx);
                if (tsqueue.empty()) return false;
                tempswapqueue.swap(tsqueue);
                return true;
            }
            void kill() {
                std::unique_lock<std::mutex> lck(mtx);
                killthreads = true;
                cv.notify_all();
            }
    };

    struct circularwritebuffer {
        size_t sentpointer = 0;
        size_t writepointer = 0;
        //count represents number of slots written
        size_t count = 0;
        uint8_t buffer[serverfields::CLIENTWRITEbufsizemax];

        bool writebytes(const void * message,size_t bytescount) {
            if (serverfields::CLIENTWRITEbufsizemax - count < bytescount) return false;
            size_t headspace = serverfields::CLIENTWRITEbufsizemax - writepointer;

            if (bytescount <= headspace) {
                memcpy(buffer + writepointer,static_cast<const uint8_t *>(message),bytescount);
                writepointer = (writepointer + bytescount) % serverfields::CLIENTWRITEbufsizemax;
            }
            else {
                memcpy(buffer + writepointer,message,headspace);
                writepointer = 0;
                memcpy(buffer + writepointer,static_cast<const uint8_t *>(message) + headspace,bytescount - headspace);
                writepointer += bytescount - headspace;
            }
            count += bytescount;
            return true;
        }

        size_t totalcontiguousfilledslots() {
            if (writepointer > sentpointer) {
                return count;
            }
            else if (writepointer <= sentpointer && count > 0) {
                return serverfields::CLIENTWRITEbufsizemax - sentpointer;
            }
            return count;
        }

        const uint8_t * getsentpointer() {
            return buffer + sentpointer;
        }

        void consumeslots(size_t n) {
            sentpointer = (sentpointer + n) % serverfields::CLIENTWRITEbufsizemax;
            count -= n;
        }
    };
}