#include <stdio.h>
#include "game.h"
#include "helpers.h"
#include <filesystem>
#include <system_error>
#include <clienterror.h>
#include "socket.h"
#include <string>

#ifndef PROJECT_ROOT_DIRECTORY
    #define PROJECT_ROOT_DIRECTORY ""
#endif

void init() {
    openlogoutputfile(PROJECT_ROOT_DIRECTORY);
    buildpaths();
    setfonts();
    buildbroadcastlist(broadcastaddrs);
}

int main() {
    if (std::string_view(PROJECT_ROOT_DIRECTORY) == "") {
        fprintf(stderr,"Root directory macro not defined, configure CMAKE\n");
        return 1;
    }

    init();
    Game g;
    g.run();
    return 0;
}