#pragma once
#include <stdint.h>
#include "proto/settings.h"

namespace messages {
    struct __attribute__((packed)) authpacket {
        uint8_t type = 2;
        char username[32];
        char password[32];
    };
    struct __attribute__((packed)) signuppacket {
        uint8_t type = 1;
        char username[network::maxcredentialsize + 1]; 
        char password[network::maxcredentialsize + 1];
        char email[network::maxemailsize + 1];
    };

    struct __attribute__((packed)) signupresponse {
        uint8_t type = 5;
        uint8_t result = 0;
    };
}