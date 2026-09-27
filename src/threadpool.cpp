#include "core/threadpool.h"
#include "server/corestructures.h"
#include <thread>
#include <vector>
#include <optional>
#include <sys/random.h>
#include "core/log.h"
extern "C" {
    #include <argon2.h>
}

hashpool::~hashpool() {
    jobqueue.kill();
    for (auto& thread:threads) {
        thread.join();
    }
}

void hashpool::submit(essential::hashentry&& job) {
    jobqueue.push(std::move(job));
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
            SERVERASSERT(job.accountid == 0);
            hres.jtype = essential::hashpooljobtype::HASH;
            essential::hashcreationparams params;

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

            SERVERASSERT(!randfailed && randres == serverfields::saltlen);

            int encoderes = argon2id_hash_encoded(serverfields::t_cost,serverfields::m_cost,serverfields::parallelism,job.password.buf,job.password.length,saltbuf,serverfields::saltlen,serverfields::hashlen,encodedbuf,sizeof encodedbuf);
            
            if (encoderes == ARGON2_OK) {
                //fill wants the actual content.
                SERVERASSERT(params.hash.fill(encodedbuf,static_cast<uint32_t>(strlen(encodedbuf))));
                params.result = essential::hashresultstatus::ENCODEDONE;
            }
            else {
                params.result = essential::hashresultstatus::ENCODEFAILED;
            }

            explicit_bzero(job.password.buf,job.password.length);
            hres.params.emplace<essential::hashcreationparams>(params);
            resqueue.push(std::move(hres));
        }

        else {
            SERVERASSERT(job.accountid != 0);
            hres.jtype = essential::hashpooljobtype::VERIFY;
            essential::hashverifyparams params;
        
            int rs = argon2_verify(job.hash.buf,job.password.buf,job.password.length,Argon2_id);
            if (rs == ARGON2_OK) {
                params.result = essential::hashresultstatus::VERIFYDONE;
                hres.accountid = job.accountid;
            }
            else {
                params.result = essential::hashresultstatus::VERIFYFAIL;
            }

            explicit_bzero(job.password.buf,job.password.length);
            hres.params.emplace<essential::hashverifyparams>(params);
            resqueue.push(std::move(hres));
        }
    }
}

bool hashpool::init(int n,essential::threadsafequeue<essential::hashresultentry>& rq,size_t len) {
    encodedlen = len;
    threads.reserve(n);
    for (int i = 0; i < n; i++) {
        threads.emplace_back(&hashpool::work,this,std::ref(rq));
    }

    return true;
}
