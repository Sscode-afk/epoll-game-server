#include "server/server.h"
#include "server/connection.h"
#include "proto/wire.h"
#include "core/log.h"
#include "proto/parser.h"
#include <vector>

void server::handlesignup(connection * c,size_t start,uint32_t size,uint8_t mtype) {
    SERVERASSERT(c->state != serverfields::connstates::NONE && c->state != serverfields::connstates::DEAD);
    
    //Ignore signup requests from connections that are not in the unauth state (already mid-signup or duplicates)
    if (c->state != serverfields::connstates::UNAUTH) return;
    
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
        messages::serversignupresponse response(network::servresponse::SIGNUPCREDTOOSHORT);
        sendmessage(c,response);
        return;
    }    
    
    SERVERASSERT(c->usernamebuf.fill(packet.username,static_cast<uint32_t>(usersize)));
    SERVERASSERT(c->passwordbuf.fill(packet.password,static_cast<uint32_t>(passsize)));
    SERVERASSERT(c->emailbuf.fill(packet.email,static_cast<uint32_t>(emailsize)));

    serverdatabase.submituserexistencejob(c->connid,c->usernamebuf);
}

void server::handlelogin(connection * c,size_t start,uint32_t size,uint8_t mtype) {
    SERVERASSERT(c->state != serverfields::connstates::NONE && c->state != serverfields::connstates::DEAD);
    if (c->state != serverfields::connstates::UNAUTH) return;

    if (static_cast<size_t>(size) != sizeof(messages::authpacket)) {
        LOGWARNING("Sign-up payload size mismatch for '%s', dropping client!",c->tag);
        markdead(c);
        return;
    }

    messages::authpacket packet;
    packet.type = mtype;
    start += 1;

    bytereader(packet.username,c->readbuffer + start,sizeof packet.username,start);
    bytereader(packet.password,c->readbuffer + start,sizeof packet.password,start);

    size_t usersize = strnlen(packet.username,sizeof packet.username);
    size_t passsize = strnlen(packet.password,sizeof packet.password);

    if (usersize == sizeof packet.username || passsize == sizeof packet.password) {
        LOGWARNING("Non null terminated character buffer(s) found in sign-up payload for '%s', dropping client!",c->tag);
        markdead(c);
        return;
    }

    if (usersize < network::mincredentialsize || passsize < network::mincredentialsize) {
        messages::serverloginresponse response(network::servresponse::LOGINCREDTOOSHORT);
        sendmessage(c,response);
        return;
    } 

    SERVERASSERT(c->usernamebuf.fill(packet.username,static_cast<uint32_t>(usersize))); //fill adds the null terminator itself
    SERVERASSERT(c->passwordbuf.fill(packet.password,static_cast<uint32_t>(passsize)));

    //push the verify database read job into the database readqueue via the submit method
    serverdatabase.submitloginauthjob(c->connid,c->usernamebuf,c->passwordbuf);
}