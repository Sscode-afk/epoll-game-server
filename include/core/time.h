#pragma once
#include <time.h>
#include <stdint.h>
#include "core/log.h"

inline uint64_t now() {
    timespec ts;
    int clockgettimesuccess = clock_gettime(CLOCK_BOOTTIME,&ts);
    SERVERASSERT(clockgettimesuccess!=-1);
    return (uint64_t)ts.tv_sec * 1000 + (ts.tv_nsec/1000000);
}