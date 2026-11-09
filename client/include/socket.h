#include <sys/socket.h>
#include <ifaddrs.h>
#include <linux/if.h>
#include <errno.h>
#include "clienterror.h"
#include <netdb.h>
#include "settings.h"
#include <vector>
#include "net.h"

inline std::vector<sockaddr_in> broadcastaddrs;

enum class udpsetuperrtype {NONE,GETSOCKET,SENDTIMEOUT,RECVTIMEOUT,SETBROADCAST};
struct udpsetupresult {
    bool success = false;
    int errcode = -1;
    udpsetuperrtype errtype = udpsetuperrtype::NONE;
};

inline udpsetupresult udpsetup(int& fd,const char * port,int timeoutsecondssend,int timeoutsecondsreceive) {
    udpsetupresult r;
    fd = getsockfd(socktype::UDP,port);
    if (fd == -1) {
        r.success = false;
        r.errtype = udpsetuperrtype::GETSOCKET;
        return r;
    }

    timeval tvSEND,tvRECV;
    tvSEND.tv_sec = timeoutsecondssend;
    tvSEND.tv_usec = 0;
    tvRECV.tv_sec = timeoutsecondsreceive;
    tvRECV.tv_usec = 0;

    if (setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tvSEND,sizeof tvSEND) == -1)  {
        r.errcode = errno;
        r.success = false;
        r.errtype = udpsetuperrtype::SENDTIMEOUT;
        return r;
    }

    if (setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&tvRECV,sizeof tvRECV) == -1) {
        r.errcode = errno;
        r.success = false;
        r.errtype = udpsetuperrtype::RECVTIMEOUT;
        return r;
    }
    
    int yes = 1;
    if (setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&yes,sizeof yes) == -1) {
        r.errcode = errno;
        r.success = false;
        r.errtype = udpsetuperrtype::SETBROADCAST;
        return r;
    }

    r.success = true;
    return r;

}

inline void buildbroadcastlist(std::vector<sockaddr_in>& interfacebroadcastaddrs) {
    ifaddrs *res,*p;
    int success;

    if ((success = getifaddrs(&res)) == -1) {
        int errnum = errno;
        CLIENTFAILNUM(errnum,"getifaddrs() failure");
    }

    for (p = res; p != nullptr; p = p->ifa_next) {
        if ((p->ifa_addr != nullptr) && (p->ifa_netmask != nullptr) && !(p->ifa_flags & IFF_LOOPBACK) && (p->ifa_flags & IFF_BROADCAST) && (p->ifa_flags & IFF_UP) && (p->ifa_addr->sa_family == AF_INET)){
            sockaddr_in * socknetmask = reinterpret_cast<sockaddr_in *>(p->ifa_netmask);
            sockaddr_in * sockinterface = reinterpret_cast<sockaddr_in *>(p->ifa_addr);

            uint32_t mask = socknetmask->sin_addr.s_addr;
            uint32_t IP = sockinterface->sin_addr.s_addr;
            uint32_t network = IP & mask;
            uint32_t broadcast = network | (~mask);

            sockaddr_in broadcastaddr{};
            broadcastaddr.sin_addr.s_addr = broadcast;
            broadcastaddr.sin_family = AF_INET;
            broadcastaddr.sin_port = htons(network::SERVERUDPPORT);

            interfacebroadcastaddrs.push_back(broadcastaddr);
        }
    }

    freeifaddrs(res);
}