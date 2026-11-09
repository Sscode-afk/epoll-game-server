#include "net.h"
#include <sys/socket.h>
#include <errno.h>
#include <cassert>
#include <stdio.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>

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

setnbresult setnonblocking(int fd) {
    setnbresult res;
    int flags = fcntl(fd,F_GETFL,0);
    if (flags == -1) {
        res.errnum = errno;
        res.success = false;
        return res;
    }

    if (fcntl(fd,F_SETFL,flags | O_NONBLOCK) == -1) {
        res.errnum = errno;
        res.success = false;
        return res;
    }

    res.success = true;
    return res;
}

int getsockfd(socktype type,const char * port) {
    addrinfo hints,*res,*p;
    memset(&hints,0,sizeof hints);
    
    //currently, only dealing with IPV4
    hints.ai_family = AF_INET;
    if (type == socktype::TCP) hints.ai_socktype = SOCK_STREAM;
    else if (type == socktype::UDP) hints.ai_socktype = SOCK_DGRAM;

    hints.ai_flags = AI_PASSIVE;

    int status;

    if ((status = getaddrinfo(nullptr,port,&hints,&res)) != 0) {
        fprintf(stderr,"gaierr: %s",gai_strerror(status));
        return -1;
    }

    int sockfd;
    int yes = 1;

    int errnum;
    for (p = res; p!=nullptr; p=p->ai_next) {
        if ((sockfd = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1) {
            errnum = errno;
            fprintf(stderr,"socket() call failed: %s",strerror(errnum));
            continue;
        }

        if (setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes) == -1) {
            errnum = errno;
            fprintf(stderr,"setsockopt on sockfd(%d) failed!",sockfd);
            close(sockfd);
            continue;
        }

        if (bind(sockfd,p->ai_addr,p->ai_addrlen) == -1) {
            errnum = errno;
            fprintf(stderr,"bind() on sockfd(%d) failed!",sockfd);
            close(sockfd);
            continue;
        }

        break;
    }

    if (p == nullptr) {
        fprintf(stderr,"socket bind failed!");
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return sockfd;
}