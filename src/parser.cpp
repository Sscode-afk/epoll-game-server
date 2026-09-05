#include <unistd.h>
#include <stdint.h>
#include <netdb.h>
#include <cstring>
#include "server/serversettings.h"
#include "proto/wire.h"
#include "proto/parser.h"

void bytereader(void * dest,void * source,size_t bytes,size_t& pointer) {
    memcpy(dest,source,bytes);
    pointer += bytes;
}

//parse increments and conditionally decrements readpointer, so not using bytereader for it
parsedata parse(uint8_t * readbuffer,size_t& readpointer,size_t& writepointer) {
    parsedata dat;
    
    if (writepointer - readpointer < 4) {
        dat.res = parseresult::INCOMPLETEMESSAGE;
        return dat;
    }

    uint32_t msgsize = 0;

    memcpy(&msgsize,readbuffer + readpointer,4);
    msgsize = ntohl(msgsize);

    if (msgsize < serverfields::MINpacketsize || msgsize > serverfields::MAXpacketsize) {
        dat.res = parseresult::OUTOFBOUNDSMSG;
        return dat;     
    };

    readpointer += 4;

    if (writepointer - readpointer < msgsize) {
        readpointer -= 4;
        dat.res = parseresult::INCOMPLETEMESSAGE;
        return dat;
    }

    uint8_t msgtype = 0;
    memcpy(&msgtype,readbuffer + readpointer,1);

    dat.payloadpointer = readpointer;
    dat.payloadsize = msgsize;
    dat.payloadtype = msgtype;
    dat.res = parseresult::SUCCESS;

    readpointer += msgsize; //parse advances past the message, so pointer arithmetic is done by parse only
    return dat;    
}
