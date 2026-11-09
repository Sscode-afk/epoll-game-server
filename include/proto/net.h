#pragma once
#include "objects.h"
#include "settings.h"
#include <arpa/inet.h>

enum class readstatus {ERROR,READBUFFERFULL,DONE,SOCKETFIN};
enum class sendbytesstatus {ERROR,SENDBUFFERFULL,BYTESSENT};
enum class flushstatus {ERROR,PARTIALFLUSH,FULLFLUSH,SOSENDBUFFERFULL};
enum class fillmessagestatus {APPLICATIONBUFFERFULL,SOCKETBUFFERFULL,PARTIALFLUSH,FULLFLUSH,ERROR,INVALID};

struct sendbytesresult {
    sendbytesstatus status;
    size_t bytessent;
};

struct readresult {
    readstatus status;
    int errcode = -1;
};

struct flushresult {
    flushstatus status;
    int errcode = -1;
};

struct fillmessageresult {
    fillmessagestatus status;
    int errcode = -1;
};

readresult fillreadbuffer(int fd, networkreadbuffer& readbuf);
sendbytesresult sendbytes(int fd,size_t tosend,circularwritebuffer& buf);
flushresult flushbytes(int fd,circularwritebuffer& buf);

template <typename Payload>
fillmessageresult fillwritebuffer(Payload& data,circularwritebuffer& buf,int fd) {
    //all fields of data are trivially copyable. sender handles the endianness of data fields
    uint32_t payloadsize = sizeof data;
    fillmessageresult res;
    
    payloadsize = htonl(payloadsize);
    if (!buf.writebytes(&payloadsize,sizeof payloadsize) || !buf.writebytes(&data,sizeof data)) {
        res.status = fillmessagestatus::APPLICATIONBUFFERFULL;
        return res;
    }
    flushresult fres = flushbytes(fd,buf);  

    switch(fres.status) {
        case flushstatus::ERROR : {
            res.errcode = fres.errcode;
            res.status = fillmessagestatus::ERROR;
            break;
        }
        case flushstatus::SOSENDBUFFERFULL : {
            res.status = fillmessagestatus::SOCKETBUFFERFULL;
            break;
        }
        case flushstatus::PARTIALFLUSH : {
            res.status = fillmessagestatus::PARTIALFLUSH;
            break;
        }
        case flushstatus::FULLFLUSH : {
            res.status = fillmessagestatus::FULLFLUSH;
            break;
        }
        default : {
            res.status = fillmessagestatus::INVALID;
            break;
        }
    }

    return res;
}

enum class socktype {TCP,UDP};
int getsockfd(socktype type,const char * port);

struct setnbresult {
    bool success = false;
    int errnum = -1;
};

setnbresult setnonblocking(int fd);
