#pragma once
#include <cstdint>
namespace network {
    inline const char * PORT = "5555";
    inline constexpr size_t mincredentialsize = 7;
    inline constexpr size_t maxcredentialsize = 15;
    inline constexpr size_t maxemailsize = 254; 

    inline const size_t MINpacketsize = 1; //single byte for type
    inline constexpr size_t MAXpacketsize = 4 * 1024; 
    
    enum class servresponse {
        SIGNUPSUCCESS = 0,
        SIGNUPCREDTOOSHORT = 1,
        SIGNUPNAMETAKEN = 2,
        SIGNUPFAILED = 3,
        LOGINFAILED = 4,
        LOGINCREDTOOSHORT = 5,
        LOGINSUCCESS = 6
    };

    inline constexpr size_t clientreadbuffersizemax = 16 * 1024;
    inline constexpr size_t clientwritebuffersizemax = 16 * 1024;

    //ENSURE THEY MATCH
    inline const uint16_t SERVERUDPPORT = 6666;
    inline const char * SERVERUDPPORTSTRING = "6666";
}