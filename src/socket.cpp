#include "server/socket.h"
#include <sys/socket.h>
#include <netdb.h>
#include <cstring>
#include "core/log.h"
#include "proto/settings.h"
#include <errno.h>
#include <fcntl.h>

#include <unistd.h>

bool setnonblocking(int fd) {
    int errnum;
    int flags = fcntl(fd,F_GETFL,0);
    if (flags == -1) {
        errnum = errno;
        LOGERRORNUM(errnum,"GETFLAGS error on socket(%d)!",fd);
        return false;
    }

    if (fcntl(fd,F_SETFL,flags | O_NONBLOCK) == -1) {
        errnum = errno;
        LOGERRORNUM(errnum,"SETFLAG nonblocking error on socket(%d)!",fd);
        return false;
    }

    return true;
}

int getlistenerfd() {
    addrinfo hints,*res,*p;
    memset(&hints,0,sizeof hints);
    
    //currently, only dealing with IPV4
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int status;
    
    if ((status = getaddrinfo(nullptr,network::PORT,&hints,&res)) != 0) {
        LOGERROR("gai err: %s",gai_strerror(status));
        return -1;
    }

    int sockfd;
    int yes = 1;

    int errnum;
    for (p = res; p!=nullptr; p=p->ai_next) {
        if ((sockfd = socket(p->ai_family,p->ai_socktype,p->ai_protocol)) == -1) {
            errnum = errno;
            LOGERRORNUM(errnum,"socket() call failed!");
            continue;
        }

        if (setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof yes) == -1) {
            errnum = errno;
            LOGERRORNUM(errnum,"setsockopt on sockfd(%d) failed!",sockfd);
            close(sockfd);
            continue;
        }

        if (bind(sockfd,p->ai_addr,p->ai_addrlen) == -1) {
            errnum = errno;
            LOGERRORNUM(errnum,"bind() on sockfd(%d) failed!",sockfd);
            close(sockfd);
            continue;
        }

        break;
    }

    if (p == nullptr) {
        LOGERROR("Listener socket bind failed!");
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    if (listen(sockfd,SOMAXCONN) == -1) {
        errnum  = errno;
        LOGERRORNUM(errnum,"listen() error on listening socket!");
        close(sockfd);
        return -1;
    }

    if (setnonblocking(sockfd) == false) return -1;
    
    return sockfd;
}