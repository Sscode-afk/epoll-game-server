#include "core/tools.h"
#include <inttypes.h>
#include <filesystem>
#include <system_error>
#include "server/serversettings.h"
#include "proto/log.h"
#include <sys/eventfd.h>

void fillIP (char * buf,size_t buflen,const sockaddr_in& addr) {
    const char * IP = inet_ntop(AF_INET,&addr.sin_addr,buf,buflen);
    if (IP == NULL) {
        snprintf(buf,buflen,"?");
    }
}

void seteventfd(int fd,const char * stage,uint64_t val) {
    ssize_t evwrite = eventfd_write(fd,val);
    if (evwrite == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"%s: eventfd write err!",stage);
        SERVERFAIL("%s: Eventfd write failure",stage);
    }

    SERVERASSERT(static_cast<size_t>(evwrite) == sizeof val,"");
}

uint64_t readeventfd(int fd,const char * stage) {
    uint64_t val;
    ssize_t bytesread = eventfd_read(fd,&val);
    if (bytesread == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"%s: eventfd read failed!",stage);
        SERVERFAIL("Closing server due to eventfd read failure!");
    }
    SERVERASSERT(static_cast<size_t>(bytesread) == sizeof val,"");
    return val;
}

void filltag (char * buf,size_t buflen,const sockaddr_in& addr,uint64_t pid) {
    char addrbuf[INET_ADDRSTRLEN];
    fillIP(addrbuf,sizeof addrbuf,addr);

    snprintf(buf,buflen,"%" PRIu64 " %s",pid,addrbuf);
}

bool buildpaths(const char * projdir) {
    std::error_code errcode;
    serverfields::databasepath = std::filesystem::path(projdir) / "database";

    if (!std::filesystem::is_directory(serverfields::databasepath,errcode)) {
        fprintf(stderr,"Specified 'assets' directory path does not exist or 'assets' is not a directory\n");
        return false;
    }
    else if (errcode) {
        fprintf(stderr,"File system error on building paths: %s\n",errcode.message());
        return false;
    }

    return true;
}