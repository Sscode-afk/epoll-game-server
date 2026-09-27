#pragma once
#include <stddef.h>
#include <type_traits>
#include <cstring>
#include <stdint.h>

template <size_t N>
struct fixedcharbuffer {
    char buf[N]{};
    size_t length = 0;
    bool fill(const char * src,uint32_t size) {
        if (size >= N) return false;
        memcpy(buf,src,static_cast<size_t>(size));
        length = size;
        buf[size] = '\0';
        return true;
    }
};