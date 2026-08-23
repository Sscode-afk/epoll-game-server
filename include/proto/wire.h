#pragma once
#include <stdint.h>

namespace messages {
    struct __attribute__((packed)) authpacket {
        uint8_t type = 1;
        char username[32];
        char password[32];
    };
}