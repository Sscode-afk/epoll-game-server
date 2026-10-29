#pragma once
#include <stddef.h>
#include <type_traits>
#include <cstring>
#include <stdint.h>
#include <cassert>
#include "settings.h"

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

struct networkreadbuffer {
    uint8_t buf[network::clientreadbuffersizemax]{};
    size_t produceindex = 0;
    size_t consumeindex = 0;
    size_t N = network::clientreadbuffersizemax;

    void compact() {
        assert(consumeindex <= produceindex);
        assert(produceindex <= N);

        if (produceindex == consumeindex) {
            produceindex = 0;
            consumeindex = 0;
        }
        else if (produceindex > consumeindex) {
            memmove(buf,buf + consumeindex,produceindex - consumeindex);
            produceindex = produceindex - consumeindex;
            consumeindex = 0;
        }
    }

    size_t getremainingslots() {
        return N - produceindex;
    }
};

struct circularwritebuffer {
    size_t consumeindex = 0;
    size_t produceindex = 0;
    //count represents number of slots written
    size_t count = 0;
    uint8_t buffer[network::clientwritebuffersizemax];

    bool writebytes(const void * message,size_t bytescount) {
        if (network::clientwritebuffersizemax - count < bytescount) return false;
        size_t headspace = network::clientwritebuffersizemax - produceindex;

        if (bytescount <= headspace) {
            memcpy(buffer + produceindex,static_cast<const uint8_t *>(message),bytescount);
            produceindex = (produceindex + bytescount) % network::clientwritebuffersizemax;
        }
        else {
            memcpy(buffer + produceindex,message,headspace);
            produceindex = 0;
            memcpy(buffer + produceindex,static_cast<const uint8_t *>(message) + headspace,bytescount - headspace);
            produceindex += bytescount - headspace;
        }
        count += bytescount;
        return true;
    }

    size_t totalcontiguousfilledslots() {
        if (produceindex > consumeindex) {
            return count;
        }
        else if (produceindex <= consumeindex && count > 0) {
            return network::clientwritebuffersizemax - consumeindex;
        }
        return count;
    }

    const uint8_t * getfirstunconsumedbytepointer() {
        return buffer + consumeindex;
    }

    void consumeslots(size_t n) {
        consumeindex = (consumeindex + n) % network::clientwritebuffersizemax;
        count -= n;
    }
};