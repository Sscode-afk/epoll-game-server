#pragma once
#include "proto/objects.h"
#include "proto/settings.h"
#include "server/corestructures.h"
#include <openssl/rand.h>
#include <sys/random.h>
#include <optional>
#include <array>

struct sessiontoken {
    //needs operator== overload for hashing
    bool operator==(const sessiontoken& othertoken) const;
    std::array<uint8_t,32> bytes;
};

struct sessiontokenpair {
    sessiontoken rawtoken;
    sessiontoken hashedtoken;
}; 

struct tokenhasher {
    size_t operator()(const sessiontoken& t) const;
};

std::optional<sessiontokenpair> gettokens();

struct usercache {
    fixedcharbuffer<network::maxcredentialsize + 1> username;
    fixedcharbuffer<network::maxemailsize + 1> email;
    uint64_t playeremblems;
};

struct Session {
    uint64_t activeconnectionid;
    uint64_t accountid;
    
    uint64_t lastseen;
    essential::tokenbucket tb;
    usercache cache;
    sessiontoken token;

    bool cachefilled = false;
    bool clearingoutboundjobs = false;
    bool sessionactive = false;
    bool dirty = false;

    uint32_t outboundasyncjobs = 0;

    Session * next = nullptr;
    Session * prev = nullptr;

    Session * dirtynext = nullptr;
    Session * dirtyprev = nullptr;

    Session(uint64_t conn,uint64_t accid,uint64_t now,sessiontoken& tok) {
        activeconnectionid = conn;
        accountid = accid;
        lastseen = now;
        token = tok;
    }  

    Session() = default;
}; 