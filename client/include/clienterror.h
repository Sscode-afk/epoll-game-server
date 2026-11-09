#pragma once
#include "log.h"
enum class clienterr {NONE, ASSET, NETWORK, SERVERDOWN};
inline clienterr err = clienterr::NONE;

inline void setclienterr(clienterr seterr) {
    if (err == clienterr::NONE) err = seterr;
}

#define CLIENTERROR(fmt,error,...) do { LOGERROR(fmt,##__VA_ARGS__); setclienterr(error); } while(0)
#define CLIENTERRORNUM(errnum,fmt,error,...) do { LOGERRORNUM(errnum,fmt,##__VA_ARGS__); setclienterr(error); } while(0)
#define CLIENTFAIL(fmt,...) do { LOGERROR(fmt,##__VA_ARGS__); std::abort(); } while(0)
#define CLIENTFAILNUM(errnum,fmt,...) do { LOGERRORNUM(errnum,fmt,##__VA_ARGS__); std::abort(); } while(0)