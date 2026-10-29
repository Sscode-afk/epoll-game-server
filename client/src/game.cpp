#include "game.h"
#include "settings.h"
#include <SFML/Graphics.hpp>

Game::Game() : window(sf::VideoMode({config::screenwidth,config::screenheight}),"Game") {};

void Game::run() {
    while (window.isOpen()) {
        while (auto event = window.pollEvent()) {
            if (event->is<sf::Event::Closed>()) {
                window.close();
            }
        }

        window.clear(config::screenbgcolor);
        window.display();
    }
}