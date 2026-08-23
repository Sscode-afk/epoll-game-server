#include <unistd.h>
#include <stdint.h>
#include <netdb.h>
#include <cstring>
#include "server/serversettings.h"
#include "proto/wire.h"
#include "proto/parser.h"

int handleauth() {
    return 0;
}
int parse(uint8_t * readbuffer,size_t& readpointer,size_t& writepointer,const char * tag) {
    if (writepointer - readpointer < 4) return -2;
    uint32_t msgsize = 0;

    memcpy(&msgsize,readbuffer + readpointer,4);
    msgsize = ntohl(msgsize);

    if (msgsize < serverfields::MINpacketsize || msgsize > serverfields::MAXpacketsize) {
        LOGWARNING("Incoming packet size out of bounds for %s, dropping!",tag);
        return -1;
    };

    readpointer += 4;

    if (writepointer - readpointer < msgsize) {
        readpointer -= 4;
        return -2;
    }

    uint8_t msgtype = 0;
    memcpy(&msgtype,readbuffer + readpointer,1);
    readpointer += 1;

    LOGINFO("Succesfully parsed incoming packet for %s!",tag);
    return static_cast<int>(msgtype);
}