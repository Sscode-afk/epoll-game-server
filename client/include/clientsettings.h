#pragma once
#include <SFML/Graphics.hpp>
#include <filesystem>
#include <optional>
#include <vector>
#include <netdb.h>

namespace config {
    inline constexpr unsigned int screenwidth = 1500;
    inline constexpr unsigned int screenheight = 1000;

    inline const sf::Color screenbgcolor = sf::Color::White;
    inline const char * updbroadcastport = "8888";

    inline unsigned int initstatebuttoncount = 0;
    inline unsigned int loginstatebuttoncount = 0;
    inline unsigned int signupstatebuttoncount = 0;
    inline unsigned int lobbystatebuttoncount = 0;
    
    inline std::filesystem::path assetpath;
    inline std::filesystem::path clientpath;

    enum class state {INIT, LOGIN, SIGNUP, LOBBY};
    enum class buttontype {initPLAYBUTTON};

    inline const char * UDPBROADCASTPORT = "9999";

    inline sf::Color inactivebuttoncolor = sf::Color({211,211,211});

    inline unsigned int errmsgsize = 45;
    inline const uint64_t udpbroadcasttimeout = 10000; //10 seconds
    inline const uint64_t udpbroadcastretry = 2000; //2 seconds

    inline const int udpsendtimeout = 2;
    inline const int udprecvtimeout = 2;

    inline std::optional<uint32_t> serverip;
}

namespace resources {
    inline std::optional<sf::Font> mainfont;
}