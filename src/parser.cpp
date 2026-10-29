#include <unistd.h>
#include <stdint.h>
#include <netdb.h>
#include <cstring>
#include "wire.h"
#include "parser.h"
#include "objects.h"

void bytereader(void * dest,void * source,size_t bytes,size_t& pointer) {
    memcpy(dest,source,bytes);
    pointer += bytes;
}

//parse increments and conditionally decrements readpointer, so not using bytereader for it
parsedata parse(networkreadbuffer& readbuf) {
    parsedata dat;
    
    if (readbuf.produceindex == readbuf.consumeindex) {
        dat.res = parseresult::ALLREAD;
        return dat;
    }

    if (readbuf.produceindex - readbuf.consumeindex < 4) {
        dat.res = parseresult::INCOMPLETEMESSAGE;
        return dat;
    }

    uint32_t msgsize = 0;

    memcpy(&msgsize,readbuf.buf + readbuf.consumeindex,4);
    msgsize = ntohl(msgsize);

    if (msgsize < network::MINpacketsize || msgsize > network::MAXpacketsize) {
        dat.res = parseresult::OUTOFBOUNDSMSG;
        return dat;     
    };

    if (readbuf.produceindex - readbuf.consumeindex < msgsize + 4) {
        dat.res = parseresult::INCOMPLETEMESSAGE;
        return dat;
    }

    readbuf.consumeindex += 4;

    uint8_t msgtype = 0;
    memcpy(&msgtype,readbuf.buf + readbuf.consumeindex,1);

    dat.payloadpointer = readbuf.consumeindex;
    dat.payloadsize = msgsize;
    dat.payloadtype = msgtype;
    dat.res = parseresult::SUCCESS;

    readbuf.consumeindex += msgsize; //parse advances past the message, so pointer arithmetic is done by parse only
    return dat;    
}