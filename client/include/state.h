#pragma once
#include <optional>
#include <SFML/Graphics.hpp>
#include "helpers.h"
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

class State {
    public:
        virtual void handlevents(std::optional<sf::Event>& event,sf::RenderWindow& window) = 0;
        virtual void handlelogic(sf::RenderWindow& window) = 0;
        virtual void handledisplay(sf::RenderWindow& window) = 0;
        
        virtual ~State() = default;
        std::optional<transitionrequest> pendingrequest;
        stateident identifier;
};

class Init : public State {
    public:
        Init(stateident ident);
        ~Init();
        void handlevents(std::optional<sf::Event>& event,sf::RenderWindow& window) override;
        void handlelogic(sf::RenderWindow& window) override;
        void handledisplay(sf::RenderWindow& window) override;
        void toauth();
        button * hoveredbutton = nullptr;
        bool freeze = false;


        void findserver();
        void broadcast();

        std::atomic<bool> broadcasting = false;
        std::optional<uint32_t> serverIP;
        std::thread broadcastthread;
        std::mutex broadcastmtx;

        uint64_t lastbroadcasttime = 0;

    private:
        button playbutton;
        std::vector<button> buttonlist;
        int udpbroadcastfd = -1;

};

class Error : public State {
    public:
        Error(const char * msg,stateident ident);
        void handlevents(std::optional<sf::Event>& event,sf::RenderWindow& window) override;
        void handlelogic(sf::RenderWindow& window) override;
        virtual void handledisplay(sf::RenderWindow& window) override;

        sf::Text errtext;
};

class Auth : public State {
    public:
        Auth();
        void handlevents(std::optional<sf::Event>& event,sf::RenderWindow& window) override;
        void handlelogic(sf::RenderWindow& window) override;
        virtual void handledisplay(sf::RenderWindow& window) override;
};   