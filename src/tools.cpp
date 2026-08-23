#include "core/tools.h"
#include <inttypes.h>

void fillIP (char * buf,size_t buflen,const sockaddr_in& addr) {
    const char * IP = inet_ntop(AF_INET,&addr.sin_addr,buf,buflen);
    if (IP == NULL) {
        snprintf(buf,buflen,"?");
    }
}
void filltag (char * buf,size_t buflen,const sockaddr_in& addr,uint64_t pid) {
    char addrbuf[INET_ADDRSTRLEN];
    fillIP(addrbuf,sizeof addrbuf,addr);

    snprintf(buf,buflen,"%" PRIu64 " %s",pid,addrbuf);
}