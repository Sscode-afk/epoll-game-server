#include "core/log.h"
#include "server/server.h"

int main() {
    setloglevel(loglevel::INFO);
    server a;
    bool initialized = a.init();
    if (!initialized) return 1;

    a.run();

    return 0;
}