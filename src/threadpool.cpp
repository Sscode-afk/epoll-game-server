#include "core/threadpool.h"
#include "server/corestructures.h"
#include <thread>
#include <vector>
#include <optional>
#include <sys/random.h>
#include "proto/log.h"
#include <sys/eventfd.h>
#include "core/tools.h"
extern "C" {
    #include <argon2.h>
}

hashpool::~hashpool() {
    stop();
}

void hashpool::stop() {
    jobqueue.kill(); //idempotent
    for (auto& thread:threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

void hashpool::work(essential::threadsafequeue<essential::hashresultentry>& resqueue) {
    char encodedbuf[serverfields::hashresultbufsize];

    while (true) {
        std::optional<essential::hashentry> result = jobqueue.popblock();
        if (result == std::nullopt) {
            //returned when kill signal is on, exit the thread
            break;
        }

        essential::hashentry job = std::move(result.value());
        uint8_t saltbuf[serverfields::saltlen];

        essential::hashresultentry hres;
        hres.id = job.connid;

        if (job.jtype == essential::hashpooljobtype::HASH) {
            //Non verification path does not set account id!
            SERVERASSERT(job.accountid == 0,"HASHPOOL: hash job already found account id set");
            hres.jtype = essential::hashpooljobtype::HASH;

            bool randfailed = false;
            ssize_t randres;

            while (true) {
                randres = getrandom(saltbuf,serverfields::saltlen,0);
                if (randres == -1) {
                    int errnum = errno;
                    if (errnum == EINTR) {
                        continue;
                    }
                    else {
                        LOGERRORNUM(errnum,"getrandom failed!");
                        randfailed = true;
                        break;                    
                    }
                }

                break;
            }

            SERVERASSERT(!randfailed && randres == serverfields::saltlen, "HASHPOOL: Random salt generation failed");

            int encoderes = argon2id_hash_encoded(serverfields::t_cost,serverfields::m_cost,serverfields::parallelism,job.password.buf,job.password.length,saltbuf,serverfields::saltlen,serverfields::hashlen,encodedbuf,sizeof encodedbuf);
            
            if (encoderes == ARGON2_OK) {
                //fill wants the actual content.
                SERVERASSERT(hres.hash.fill(encodedbuf,static_cast<uint32_t>(strlen(encodedbuf))),"HASHPOOL: Failed filling bytes into the result hash buffer from the encoded buffer");
                hres.status = essential::hashresultstatus::ENCODEDONE;
            }
            else {
                hres.status = essential::hashresultstatus::ENCODEFAILED;
            }

            explicit_bzero(job.password.buf,job.password.length);
            resqueue.push(std::move(hres));
        }

        else {
            SERVERASSERT(job.accountid != 0, "HASHPOOL: Verification job must have an account id to propogate to the result, account id not already set");
            hres.jtype = essential::hashpooljobtype::VERIFY;
        
            int rs = argon2_verify(job.hash.buf,job.password.buf,job.password.length,Argon2_id);
            if (rs == ARGON2_OK) {
                hres.status = essential::hashresultstatus::VERIFYDONE;
                hres.accountid = job.accountid;
            }
            else {
                hres.status = essential::hashresultstatus::VERIFYFAIL;
            }

            explicit_bzero(job.password.buf,job.password.length);
            resqueue.push(std::move(hres));
        }

        //After pushing in either case, notify the main thread
        seteventfd(resulteventfd,"HASHPOOL");
    }
}

bool hashpool::init(int n,essential::threadsafequeue<essential::hashresultentry>& rq,size_t len, int efd) {
    encodedlen = len;
    resulteventfd = efd;

    SERVERASSERT(encodedlen > 0, "HASHPOOL: encodedlen parameter is invalid (0)");
    SERVERASSERT(resulteventfd >= 0, "HASHPOOL: resulteventfd parameter is invalid");

    threads.reserve(n);
    for (int i = 0; i < n; i++) {
        threads.emplace_back(&hashpool::work,this,std::ref(rq));
    }

    return true;
}

void hashpool::submithashcreationjob(uint64_t connid,fixedcharbuffer<network::maxcredentialsize + 1>& password) {
    essential::hashentry h;
    h.connid = connid;
    h.password = password;
    h.jtype = essential::hashpooljobtype::HASH;
    SERVERASSERT(h.hash.length == 0 && h.accountid == 0, "HASHPOOL: hash creation job must have accountid and hash fields empty");

    jobqueue.push(std::move(h));
}

void hashpool::submithashverificationjob(uint64_t connid,uint64_t accid,fixedcharbuffer<network::maxcredentialsize + 1>& password,fixedcharbuffer<serverfields::hashresultbufsize>& hash) {
    SERVERASSERT(accid > 0, "HASHPOOL: hash verification job must have accountid parameter non zero");
    essential::hashentry h;
    h.connid = connid;
    h.accountid = accid;
    h.jtype = essential::hashpooljobtype::VERIFY;
    h.password = password;

    //verification readjob ensures that the hash length is correct!
    h.hash = hash;
    
    jobqueue.push(std::move(h));
}