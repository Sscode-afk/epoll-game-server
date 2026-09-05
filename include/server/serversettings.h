#pragma once
#include <unistd.h>
namespace serverfields {
    inline const size_t EPEVENTbufsize = 128;
    inline constexpr size_t CLIENTWRITEbufsizemax = 16 * 1024; 
    inline constexpr size_t CLIENTREADbufsizemax = 16 * 1024;

    inline size_t MINpacketsize = 1; //single byte for type
    inline constexpr size_t MAXpacketsize = 4 * 1024; 

    inline size_t MAXipconcurrentusers = 25;
    inline double iptbcap = 15;
    inline double iptbrefillrate = 0.0005; //per millisecond

    inline double accepttokencost = 1.0;
    inline uint64_t ipsweepinterval = 4000; //4 seconds

    enum class connstates {NONE,UNAUTH,IDLE,INMATCH,DEAD};

    inline const int MAXunauthconnlimit = 2000;
    inline const int MAXauthconnlimit = 8000;

    inline uint64_t authtimeout = 5000; //5 seconds
    inline uint64_t idletimeout = 5000;

    inline constexpr size_t hashresultbufsize = 128; //asserted against the actual encoded_len
    inline int hashpoolthreadcount = 4;
    inline constexpr uint32_t t_cost = 3;
    inline constexpr uint32_t m_cost = 38912;
    inline constexpr uint32_t parallelism = 1;
    inline constexpr uint32_t hashlen = 32;
    inline constexpr uint32_t saltlen = 16;
}