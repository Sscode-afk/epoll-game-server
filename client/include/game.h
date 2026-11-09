#include <SFML/Graphics.hpp>
#include "state.h"
#include "memory"
class Game {
    public:
        sf::RenderWindow window;
        Game();
        ~Game();
        bool init();
        void run();
        void checkerr();
    private:
        int UDPbroadcastfd = 0;
        std::unique_ptr<State> currentstate;
        void changestate(transitionrequest& t);
};