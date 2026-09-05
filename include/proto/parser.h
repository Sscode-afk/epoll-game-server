#pragma once
#include <unistd.h>
#include <stdint.h>

enum class parseresult {OUTOFBOUNDSMSG,INCOMPLETEMESSAGE,SUCCESS};
void bytereader(void * dest,void * source,size_t bytes,size_t& pointer);
struct parsedata {
    uint8_t payloadtype = 0;
    size_t payloadpointer = 0;
    uint32_t payloadsize = 0;
    parseresult res;
};

parsedata parse(uint8_t* readbuffer,size_t& readpointer,size_t& writepointer);