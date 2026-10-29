#pragma once
#include <unistd.h>
namespace serverfields {
    inline const size_t EPEVENTbufsize = 128;
    
    inline const size_t MAXipconcurrentusers = 25;
    inline const double iptbcap = 15;
    inline const double iptbrefillrate = 0.0005; //per millisecond

    inline const double accepttokencost = 1.0;
    inline const uint64_t ipsweepinterval = 4000; //4 seconds

    enum class connstates {NONE,UNAUTH,IDLE,INMATCH,DEAD,LOGINDB,LOGINHASH,SIGNUPDBCHECK,SIGNUPHASH,SIGNUPDBWRITE,SESSIONCACHEGET,TOTALSTATES};
    constexpr size_t totalstatescount = static_cast<size_t>(connstates::TOTALSTATES);

    inline const int MAXunauthconnlimit = 2000;
    inline const int MAXauthconnlimit = 8000;

    inline const uint64_t authtimeout = 5000; //5 seconds
    inline const uint64_t idletimeout = 5000;

    inline constexpr size_t hashresultbufsize = 128; //asserted against the actual encoded_len
    inline constexpr int hashpoolthreadcount = 4;
    inline constexpr uint32_t t_cost = 3;
    inline constexpr uint32_t m_cost = 38912;
    inline constexpr uint32_t parallelism = 1;
    inline constexpr uint32_t hashlen = 32;
    inline constexpr uint32_t saltlen = 16;

    //NONE,UNAUTH,IDLE,INMATCH,DEAD,LOGINDB,LOGINHASH,SIGNUPDBCHECK,SIGNUPHASH,SIGNUPDBWRITE,SESSIONCACHEGET
    constexpr bool transitiontable[totalstatescount][totalstatescount] = {
        {0,1,0,0,1,0,0,0,0,0,0}, //NONE
        {0,0,1,0,1,1,0,1,0,0,1}, //UNAUTH
        {0,0,0,1,1,0,0,0,0,0,0}, //IDLE
        {0,0,1,0,1,0,0,0,0,0,0}, //INMATCH
        {0,0,0,0,0,0,0,0,0,0,0}, //DEAD
        {0,1,0,0,1,0,1,0,0,0,0}, //LOGINDB
        {0,1,0,0,1,0,0,0,0,0,1}, //LOGINHASH
        {0,1,0,0,1,0,0,0,1,0,0}, //SIGNUPDBCHECK
        {0,1,0,0,1,0,0,0,0,1,0}, //SIGNUPHASH
        {0,1,0,0,1,0,0,0,0,0,1}, //SIGNUPDBWRITE 
        {0,0,1,0,1,0,0,0,0,0,0}  //SESSIONCACHEGET
    };

    enum class sessioncreationstage {FROMLOGIN,FROMSIGNUP,FROMTOKEN};

    inline const uint64_t sessiontimeout = 172800000; //2 days in ms
}