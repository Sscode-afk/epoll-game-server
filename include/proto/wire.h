#pragma once
#include <stdint.h>
#include "settings.h"

namespace messages {
    struct __attribute__((packed)) authpacket {
        uint8_t type = 2;
        char username[network::maxcredentialsize + 1];
        char password[network::maxcredentialsize + 1];
    };
    struct __attribute__((packed)) signuppacket {
        uint8_t type = 1;
        char username[network::maxcredentialsize + 1]; 
        char password[network::maxcredentialsize + 1];
        char email[network::maxemailsize + 1];
    };

    struct __attribute__((packed)) serversignupresponse {
        uint8_t type = 5;
        uint8_t result = 0;
        serversignupresponse(network::servresponse response) {
            result = static_cast<uint8_t>(response);
        }
    };
    struct __attribute__((packed)) serverloginresponse {
        uint8_t type = 6;
        uint8_t result;
        serverloginresponse(network::servresponse response) {
            result = static_cast<uint8_t>(response);
        }
    };

    struct __attribute__((packed)) servershutdownpacket {
        uint8_t type = 10;
    };

    struct __attribute__((packed)) servernewconntakeoverpacket {
        uint8_t type = 11;
    };

    struct __attribute__((packed)) serversessiontokenpacket {
        uint8_t type = 12;
        uint8_t token[32];
    };

    struct __attribute__((packed)) serverlobbyinfo {
        char username[network::maxcredentialsize + 1];
        char email[network::maxemailsize + 1];
        uint64_t emblems;
    };
}