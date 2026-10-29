#pragma once
#include <stdint.h>
#include <netdb.h>
#include <vector>
#include "server/serversettings.h"
#include "proto/objects.h"
#include "proto/settings.h"
#include "server/session.h"

enum class epollfdtype {LISTENER,RESULT,CONNECTION};

struct basefd {
    epollfdtype type;
};

struct connection : public basefd {
    connection() {
        connfd = -1;
    }
    ~connection() {
        if (connfd >=0) close(connfd);
    }

    connection(const connection&) = delete;
    connection& operator= (const connection&) = delete;

    uint64_t connid;
    int connfd;

    bool alive;
    bool polloutactive;

    sockaddr_in connaddr;

    networkreadbuffer readbuffer;
    circularwritebuffer writebuffer;

    char tag[4 + 20 + INET_ADDRSTRLEN + 2]; //"conn {max uint64t} {maxipv4addr}\0"

    serverfields::connstates state = serverfields::connstates::NONE;
    uint64_t lastseen;

    connection * next = nullptr;
    connection * prev = nullptr;

    fixedcharbuffer<network::maxcredentialsize+1> usernamebuf;
    fixedcharbuffer<network::maxcredentialsize+1> passwordbuf;
    fixedcharbuffer<network::maxemailsize + 1> emailbuf;

    Session * sess = nullptr;
};