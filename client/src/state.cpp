#include "state.h"
#include "settings.h"
#include <unistd.h>
#include <vector>
#include "net.h"
#include <string>
#include "helpers.h"
#include "wire.h"
#include "socket.h"
#include "timer.h"
#include <thread>

Init::Init(stateident ident) : playbutton(config::state::INIT,config::buttontype::initPLAYBUTTON,150.f,70.f,sf::Color::Red,sf::Color::White,resources::mainfont.value(),"Play",50,2.f) 
{
    identifier = ident;
    playbutton.placebutton(config::screenwidth/2.f,config::screenheight/2.f,originposition::CENTER);
    buttonlist.push_back(playbutton);
    assert(buttonlist.size() == config::initstatebuttoncount);
}

Init::~Init() {
    if (broadcastthread.joinable()) broadcastthread.join();
    if (udpbroadcastfd >=0) close(udpbroadcastfd);
}

void Init::broadcast() {
    int retries = 5;
    messages::clientudpdiscoverypacket packet;
    sockaddr_in serveraddr;
    socklen_t addrsize;


    while (retries > 0) {
        for (auto& baddr:broadcastaddrs) {
            ssize_t bytessent = sendto(udpbroadcastfd,&packet,sizeof packet,MSG_NOSIGNAL,reinterpret_cast<sockaddr *>(&baddr),sizeof baddr);
            if (bytessent == -1) {
                int errnum = errno;
                CLIENTERRORNUM(errnum,"UDP socket send err",clienterr::NETWORK);
                return;
            }
        }

        addrsize = sizeof serveraddr;
        messages::serverudpresponse receivepacket;

        ssize_t bytesreceived = recvfrom(udpbroadcastfd,&receivepacket,sizeof receivepacket,0,(sockaddr *)&serveraddr,&addrsize);
        if (bytesreceived == -1) {
            int errnum = errno;
            if (errnum == EAGAIN || errnum == EWOULDBLOCK) {
                retries--;
                continue;
            }
            else {
                CLIENTERRORNUM(errnum,"UDP socket recv err",clienterr::NETWORK);
                return;
            }
        }
        else {
            //UDP ensures the entire packet arrived provided the buffer is large enough
            if (receivepacket.type == 21) {
                {
                    std::lock_guard<std::mutex> lck(broadcastmtx);
                    serverIP.emplace(serveraddr.sin_addr.s_addr);
                    return;
                }
            }
        }
    }

    //reaching here means server could not be found
    CLIENTERROR("Could not receive server IP",clienterr::SERVERDOWN);

}

void Init::findserver() {
    udpsetupresult r = udpsetup(udpbroadcastfd,config::UDPBROADCASTPORT,config::udpsendtimeout,config::udprecvtimeout);
    if (!r.success) {
        switch (r.errtype) {
            case udpsetuperrtype::GETSOCKET : {
                CLIENTERROR("Could not setup UDP socket",clienterr::NETWORK);
                return;
            }
            case udpsetuperrtype::SENDTIMEOUT : {
                CLIENTERRORNUM(r.errcode,"UDP socket send timeout setsockopt err",clienterr::NETWORK);
                return;
            }
            case udpsetuperrtype::RECVTIMEOUT : {
                CLIENTERRORNUM(r.errcode,"UDP socket recv timeout setsockopt err",clienterr::NETWORK);
                return;               
            }
            case udpsetuperrtype::SETBROADCAST : {
                CLIENTERRORNUM(r.errcode,"UDP socket broadcast setsockopt err",clienterr::NETWORK);
                return;
            }
        }
    }
    if (!broadcastthread.joinable()) broadcastthread = std::thread(&Init::broadcast,this);
}

void Init::handlevents(std::optional<sf::Event>& event,sf::RenderWindow& window) {
    if (freeze) return;
    if (event->is<sf::Event::MouseMoved>()) {
        buttonmousecollision(buttonlist,window,hoveredbutton);
    }
    if (event->is<sf::Event::MouseButtonPressed>()) {
        if (hoveredbutton) {
            switch (hoveredbutton->type) {
                case config::buttontype::initPLAYBUTTON : {
                    freezebuttons(buttonlist);
                    broadcasting = true;
                    freeze = true;
                    findserver();
                }
            }
        }
    }

}

void Init::toauth() {
    transitionrequest t;
    t.from = identifier;
    t.to = stateident::AUTH;

    pendingrequest.emplace(t);
}

void Init::handlelogic(sf::RenderWindow& window) {
    if (broadcasting) {
        std::lock_guard<std::mutex> lck(broadcastmtx);
        if (serverIP) {
            config::serverip.emplace(serverIP.value());
        toauth();
        }
    }
}

void Init::handledisplay(sf::RenderWindow& window) {
    window.clear(config::screenbgcolor);
    for (auto& button:buttonlist) {
        button.draw(window);
    }
}

////////

Error::Error(const char * errmsg,stateident ident) : errtext(resources::mainfont.value()) {
    identifier = ident;

    errtext.setCharacterSize(config::errmsgsize);
    errtext.setString(errmsg);
    errtext.setFillColor(sf::Color::Black);
    setorigin(errtext,originposition::CENTER);
    errtext.setPosition({config::screenwidth/2.f,config::screenheight/2.f});
}

void Error::handledisplay(sf::RenderWindow& window) {
    window.clear(sf::Color::White);
    window.draw(errtext);
}

void Error::handlelogic(sf::RenderWindow& window) {}
void Error::handlevents(std::optional<sf::Event>& event,sf::RenderWindow& window) {}

/////////
Auth::Auth() {

}

void Auth::handlevents(std::optional<sf::Event>& event,sf::RenderWindow& window) {}
void Auth::handlelogic(sf::RenderWindow& window) {}
void Auth::handledisplay(sf::RenderWindow& window) {
    window.clear(sf::Color::Red);
}