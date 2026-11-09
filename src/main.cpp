#include "proto/log.h"
#include "server/server.h"
#include "core/tools.h"

#ifndef PROJECT_ROOT_DIRECTORY
    #define PROJECT_ROOT_DIRECTORY ""
#endif

int main() {
    setloglevel(loglevel::INFO);
    if (!buildpaths(PROJECT_ROOT_DIRECTORY)) {
        LOGERROR("Could not build paths successfully");
        return 1;
    }
    
    server a;
    bool initialized = a.init();
    if (!initialized) return 1;
    
    a.run();
    return 0;
}