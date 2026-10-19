#include "server/session.h"
#include <optional>
#include <sys/random.h>
#include <openssl/sha.h>
#include <cstring>

std::optional<sessiontokenpair> gettokens() {
    sessiontokenpair res;
    
    while (true) {
        ssize_t rs = getrandom(res.rawtoken.bytes.data(),sizeof res.rawtoken.bytes,0);
        if (rs == -1) {
            int errnum = errno;
            if (errnum == EINTR) {
                continue;
            }
            else {
                return std::nullopt;
            }
        }

        if (rs != sizeof res.rawtoken.bytes) return std::nullopt;
        SHA256(res.rawtoken.bytes.data(),sizeof res.rawtoken.bytes,res.hashedtoken.bytes.data());

        return res;
    }
}

bool sessiontoken::operator==(const sessiontoken& othertoken) const{
    return bytes == othertoken.bytes;
}

size_t tokenhasher::operator()(const sessiontoken& t) const {
    size_t hashedkey;
    memcpy(&hashedkey,t.bytes.data(),sizeof hashedkey);
    return hashedkey;
}