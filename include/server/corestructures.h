#pragma once
#include "server/serversettings.h"
#include <algorithm>
namespace essential {
    struct tokenbucket {
        uint64_t lastseen;
        double tokens;
        double capacity;
        double refillrate;
    };

    struct IP {
        size_t count = 0;
        tokenbucket tb = {0,serverfields::iptbcap,serverfields::iptbcap,serverfields::iptbrefillrate};

        IP(uint64_t createdat) {
            tb.lastseen = createdat;
        }
    };

    inline void updatetokens(tokenbucket& tb,uint64_t now) {
        tb.tokens = std::clamp(tb.tokens + (now - tb.lastseen) * tb.refillrate,0.0,tb.capacity);
        tb.lastseen = now;
    }
}