#pragma once
#include <time.h>
#include <stdint.h>
#include "log.h"
#include <inttypes.h>

enum class timerprecision {MILLI,MICRO};
inline uint64_t now(timerprecision type = timerprecision::MILLI) {
    timespec ts;
    int clockgettimesuccess = clock_gettime(CLOCK_BOOTTIME,&ts);
    SERVERASSERT(clockgettimesuccess!=-1,"clock_gettime failed");

    if (type == timerprecision::MILLI)  return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec/1000000);
    return (uint64_t)ts.tv_sec * 1000000 + (ts.tv_nsec/1000);
}

struct measure_t {
    public:
        uint64_t current;
        timerprecision mtype;
        const char * val;

        bool logged = false;
        measure_t(const char * task,timerprecision type = timerprecision::MILLI) {
            current = now(type);
            mtype = type;
            val = task;
        }
        ~measure_t() {
            if (!logged) log();
        }
        void end() {
            log();
            logged = true;
        }
        void log() {
            current = now(mtype) - current;
            const char * unit = (mtype == timerprecision::MILLI ? "ms":"µs");
            LOGINFO("%s took: %" PRIu64 " %s.",val,current,unit);
        }
};
