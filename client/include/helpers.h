#pragma once
#include <SFML/Graphics.hpp>
#include "clientsettings.h"
#include <vector>
#include <system_error>
#include <variant>
#include <string>
#include "clienterror.h"
#include <fcntl.h>

enum class originposition {TOPLEFT,BOTTOMLEFT,CENTER};

template <typename sfobject>
void setorigin(sfobject& obj,originposition pos) {
    sf::FloatRect bounds = obj.getLocalBounds();
    switch (pos) {
        case originposition::CENTER : {
            obj.setOrigin({bounds.position.x + bounds.size.x/2.f,bounds.position.y + bounds.size.y/2.f});
            break;
        }
        case originposition::TOPLEFT : {
            obj.setOrigin({bounds.position.x,bounds.position.y});
            break;
        }
        case originposition::BOTTOMLEFT : {
            obj.setOrigin({bounds.position.x,bounds.position.y + bounds.size.y});
            break;
        }
    }
}

struct button {
    sf::RectangleShape shape;
    sf::Text text;
    originposition opos = originposition::CENTER;
    bool active = true;
    bool hover = false;
    config::buttontype type;

    sf::Color shapeoriginalcolor;
    sf::Color textoriginalcolor;

    button(config::state state,config::buttontype btype,float width,float height,sf::Color color,sf::Color textcolor,const sf::Font& font,const char * textcontent,unsigned int textsize,float outlinethickness = 0.f)
    : shape({width,height}), text(font) 
    {
        shape.setFillColor(color);
        shape.setOutlineColor(sf::Color::Black);
        shape.setOutlineThickness(outlinethickness);
        text.setString(textcontent);
        text.setCharacterSize(textsize);
        text.setFillColor(textcolor);

        setorigin(text,originposition::CENTER);
        setorigin(shape,opos);

        text.setPosition({shape.getPosition().x,shape.getPosition().y});
        
        shapeoriginalcolor = color;
        textoriginalcolor = textcolor;
        type = btype;

        switch (state) {
            case config::state::INIT : {
                config::initstatebuttoncount++;
                break;
            }
            case config::state::LOGIN : {
                config::loginstatebuttoncount++;
                break;
            }
            case config::state::SIGNUP : {
                config::signupstatebuttoncount++;
                break;
            }
            case config::state::LOBBY : {
                config::lobbystatebuttoncount++;
                break;
            }
        }

    }

    void placebutton(float x, float y,originposition originpos) {
        if (opos != originpos) {
            setorigin(shape,originpos);
            opos = originpos;
        }
        shape.setPosition({x,y});   
        float shapewidth = shape.getSize().x;
        float shapeheight = shape.getSize().y;
        switch (originpos) {
            case originposition::TOPLEFT : {
                text.setPosition({x + shapewidth/2.f,y + shapeheight/2.f});
                break;
            }
            case originposition::CENTER : {
                text.setPosition({x,y});
                break;
            }
            case originposition::BOTTOMLEFT : {
                text.setPosition({x + shapewidth/2.f,y - shapeheight/2.f});
                break;
            }
        }
    }

    void draw(sf::RenderWindow& window) {
        window.draw(shape);
        window.draw(text);
    }
};

inline void buttonmousecollision(std::vector<button>& buttonlist,sf::RenderWindow& window,button*& hoveredbutton) {
    sf::Vector2i mousepos = sf::Mouse::getPosition(window);
    hoveredbutton = nullptr;
    for (auto& button:buttonlist) {
        bool hover = button.active && button.shape.getGlobalBounds().contains(sf::Vector2f(mousepos));
        button.hover = hover;
        
        button.shape.setFillColor(hover ? sf::Color::White : button.shapeoriginalcolor);
        button.text.setFillColor(hover ? sf::Color::Black : button.textoriginalcolor);

        if (hover) hoveredbutton = &button;
    }
}

inline void freezebuttons(std::vector<button>& buttonlist) {
    for (auto& button:buttonlist) {
        button.shape.setFillColor(config::inactivebuttoncolor);
        button.text.setFillColor(sf::Color::Black);
    }
}

inline void openlogoutputfile(const char * projdir) {
    std::error_code errcode;
    config::clientpath = std::filesystem::path(projdir) / "client";
    bool valid = std::filesystem::is_directory(config::clientpath,errcode);

    if (errcode) {
        CLIENTFAIL("File system error on building paths: %s",errcode.message().c_str());
    }
    else if (!valid) {
        CLIENTFAIL("Specified 'client' directory path does not exist or 'client' is not a directory");
    }

    std::filesystem::path logoutputpath = std::filesystem::path(config::clientpath) / "log.txt";
    int lfd = open(logoutputpath.c_str(),O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (lfd == -1) {
        int errnum = errno;
        CLIENTFAILNUM(errnum,"Could not open log file ( %s )",logoutputpath.c_str());
    }
    setlogfd(lfd);
}
inline void buildpaths() {
    std::error_code errcode;
    config::assetpath = std::filesystem::path(config::clientpath) / "assets";
    bool validassets = std::filesystem::is_directory(config::assetpath,errcode);

    if (errcode) {
        CLIENTFAIL("File system error on building paths: %s",errcode.message().c_str());
    }
    else if (!validassets) {
        CLIENTFAIL("Specified 'assets' directory path does not exist or 'assets' is not a directory");
    }
}

inline void setfonts() {
    std::error_code errcode;
    std::filesystem::path mainfontpath = std::filesystem::path(config::assetpath) / "Handjet-Regular.ttf";

    bool valid = std::filesystem::is_regular_file(mainfontpath,errcode);
    
    if (errcode) {
        CLIENTFAIL("File system error on checking font path: %s",errcode.message().c_str());
    }
    else if (!valid) {
        CLIENTFAIL("Could not find main font in assets folder");
    }
    else resources::mainfont.emplace(mainfontpath);
}

//state helpers and structs
enum class stateident {INIT,ERROR,AUTH};

struct inittoerrordata {
    std::string msg;
    inittoerrordata(const std::string& s) {
        msg = s;
    }
};

struct transitionrequest {
    stateident from;
    stateident to;
    std::variant<std::monostate,inittoerrordata> data;
};