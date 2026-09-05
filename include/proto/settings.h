#pragma once
namespace network {
    inline const char * PORT = "5555";
    inline constexpr size_t mincredentialsize = 7;
    inline constexpr size_t maxcredentialsize = 15;
    inline constexpr size_t maxemailsize = 254; 

    enum class servresponse {SIGNUPSUCCESS = 0,SIGNUPCREDTOOSHORT = 1,SIGNUPNAMETAKEN = 2};
}