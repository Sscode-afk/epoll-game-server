#include "server/server.h"
#include "server/connection.h"
#include "proto/wire.h"
#include "core/log.h"
#include "proto/parser.h"

void server::handlesignup(connection * c,size_t start,uint32_t size,uint8_t mtype) {
    if (static_cast<size_t>(size) != sizeof(messages::signuppacket)) {
        LOGWARNING("Sign-up payload size mismatch for '%s', dropping client!",c->tag);
        markdead(c);
        return;
    }
    messages::signuppacket packet;
    packet.type = mtype;
    start += 1;

    bytereader(packet.username,c->readbuffer + start,sizeof packet.username,start);
    bytereader(packet.password,c->readbuffer + start,sizeof packet.password,start);
    bytereader(packet.email,c->readbuffer + start,sizeof packet.email,start);
    
    size_t usersize = strnlen(packet.username,sizeof packet.username);
    size_t passsize = strnlen(packet.password,sizeof packet.password);
    size_t emailsize = strnlen(packet.email,sizeof packet.email);
    
    if (usersize == sizeof packet.username || passsize == sizeof packet.password || emailsize == sizeof packet.email ) {
        LOGWARNING("Non null terminated character buffer(s) found in sign-up payload for '%s', dropping client!",c->tag);
        markdead(c);
        return;
    }

    if (usersize < network::mincredentialsize || passsize < network::mincredentialsize) {
        messages::signupresponse response;
        response.result = static_cast<int>(network::servresponse::SIGNUPCREDTOOSHORT);
        sendmessage(c,response);
        return;
    }

    //DB thread insertion and subsequent hashjob
    
}