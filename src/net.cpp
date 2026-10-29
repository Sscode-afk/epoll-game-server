#include "net.h"
#include <sys/socket.h>
#include <errno.h>
#include <cassert>


sendbytesresult sendbytes(int fd, size_t tosend,circularwritebuffer& buf) {
    sendbytesresult res;
    ssize_t bytessent = send(fd,buf.getfirstunconsumedbytepointer(),tosend,MSG_NOSIGNAL);
    if (bytessent == -1) {
        int errnum = errno;
        if (errnum == EAGAIN || errnum == EWOULDBLOCK) {
            //send buffer is already full
            res.bytessent = 0;
            res.status = sendbytesstatus::SENDBUFFERFULL;
            return res;
        }
        else {
            res.status = sendbytesstatus::ERROR;
            res.bytessent = errnum;
            return res;
        }
    }

    res.status = sendbytesstatus::BYTESSENT;
    res.bytessent = static_cast<size_t>(bytessent);
    return res;
}

flushresult flushbytes(int fd,circularwritebuffer& buf) {
    size_t cslots = buf.totalcontiguousfilledslots();
    sendbytesresult sendres = sendbytes(fd,cslots,buf);

    flushresult flushres;

    if (sendres.status == sendbytesstatus::ERROR) {
        flushres.status = flushstatus::ERROR;
        flushres.errcode = static_cast<int>(sendres.bytessent);
        return flushres; 
    }

    else if (sendres.status == sendbytesstatus::SENDBUFFERFULL) {
        flushres.status = flushstatus::SOSENDBUFFERFULL;
        return flushres;        
    }

    else if (sendres.status == sendbytesstatus::BYTESSENT) {
        size_t filledslotspreconsumption = buf.count;
        buf.consumeslots(sendres.bytessent);

        if (sendres.bytessent < cslots) {
            flushres.status = flushstatus::PARTIALFLUSH;
            return flushres;
        }

        //all contiguous slots sent, if these are the same as all the filled slots, then the work is done
        if (cslots == filledslotspreconsumption) {
            flushres.status = flushstatus::FULLFLUSH;
            return flushres;
        }
        
        //if reached here, consumeindex MUST be at 0
        assert(buf.consumeindex == 0);

        flushres = flushbytes(fd,buf);
        if (flushres.status == flushstatus::PARTIALFLUSH || flushres.status == flushstatus::SOSENDBUFFERFULL) {
            flushres.status = flushstatus::PARTIALFLUSH;
            return flushres;
        }
        else return flushres;
    }
}

readresult fillreadbuffer(int fd,networkreadbuffer& readbuffer) {
    readresult r;
    while (true) {
        size_t remaining = readbuffer.getremainingslots();
        if (remaining == 0) {
            r.status = readstatus::READBUFFERFULL;
            return r;
        }

        ssize_t bytesfilled = recv(fd,readbuffer.buf + readbuffer.produceindex,remaining,0);
        
        if (bytesfilled == -1) {
            int errnum = errno;
            if (errnum == EAGAIN || errnum == EWOULDBLOCK) {
                //socket recv buffer fully drained
                r.status = readstatus::DONE;
                return r;
            }
            else {
                r.status = readstatus::ERROR;
                r.errcode = errnum;
                return r;
            }
        }
        else if (bytesfilled == 0) {
            r.status = readstatus::SOCKETFIN;
            return r;
        }

        readbuffer.produceindex += static_cast<size_t>(bytesfilled);
    }
}