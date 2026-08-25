#pragma once
#include <stdint.h>
#include <netdb.h>
#include <vector>
#include "server/serversettings.h"
struct connection {
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

    uint8_t readbuffer[serverfields::CLIENTREADbufsizemax];
    uint8_t writebuffer[serverfields::CLIENTWRITEbufsizemax];

    char tag[4 + 20 + INET_ADDRSTRLEN + 2]; //"conn {max uint64t} {maxipv4addr}\0"
    size_t readindex;
    size_t writeindex;

    serverfields::connstates state = serverfields::connstates::NONE;
    uint64_t lastseen;

    connection * next = nullptr;
    connection * prev = nullptr;
};