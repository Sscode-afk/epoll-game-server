#include "game.h"
#include "clientsettings.h"
#include <SFML/Graphics.hpp>
#include "net.h"
#include "state.h"
#include <unistd.h>

Game::Game() : window(sf::VideoMode({config::screenwidth,config::screenheight}),"Game",sf::Style::Titlebar | sf::Style::Close) {
    window.setFramerateLimit(120);
    window.setVerticalSyncEnabled(true);
    currentstate = std::make_unique<Init>(stateident::INIT);
};

void Game::changestate(transitionrequest& t) {
    if (currentstate->identifier != t.from) return;
    if (t.from == stateident::INIT) {
        switch (t.to) {
            case stateident::ERROR : {
                inittoerrordata * dat = std::get_if<inittoerrordata>(&t.data);
                if (dat) {
                    currentstate = std::make_unique<Error>(dat->msg.c_str(),stateident::ERROR);
                }
                break;
            }
            case stateident::AUTH : {
                currentstate = std::make_unique<Auth>();
            }
        }
    }
}

void Game::checkerr() {
    if (err == clienterr::NONE) return;
    const char * msg;
    switch (err) {
        case clienterr::NETWORK : {
            msg = "Some network error occured.";
            break;
        }
        case clienterr::SERVERDOWN : {
            msg = "Server down.";
            break;
        }
        default : {
            CLIENTFAIL("UNKNOWN err state set");
        }
    }

    currentstate = std::make_unique<Error>(msg,stateident::ERROR);
}
void Game::run() {
    while (window.isOpen()) {
        checkerr();
        
        if (currentstate->pendingrequest) {
            changestate(currentstate->pendingrequest.value());
        }
        while (auto event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            }
            currentstate->handlevents(event,window);
        }

        currentstate->handlelogic(window);
        currentstate->handledisplay(window);
        window.display();
    }
}

bool Game::init() {
    if ((UDPbroadcastfd = getsockfd(socktype::UDP,config::updbroadcastport)) == -1) return false;
    return true;
}

Game::~Game() {
    if (logfd >= 0) close(logfd);
}