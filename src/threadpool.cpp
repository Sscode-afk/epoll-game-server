#include "core/threadpool.h"
#include "server/corestructures.h"
#include <thread>
#include <vector>
#include <optional>
#include <sys/random.h>
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
    std::vector<char> encodedbuffer;
    encodedbuffer.resize(encodedlen);

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
            bool randfailed = false;
            while (true) {
                int randres = getrandom(saltbuf,serverfields::saltlen,0);
                if (randres == -1) {
                    int errnum = errno;
                    if (errnum == EINTR) {
                        continue;
                    }
                    else {
                        randfailed = true;
                        break;                    
                    }
                }
            }

            if (randfailed) {
                hres.result = essential::hashresultstatus::RANDFAILED;
                resqueue.push(std::move(hres));
                continue;
            }

            int encoderes = argon2id_hash_encoded(serverfields::t_cost,serverfields::m_cost,serverfields::parallelism,job.pwdbuf,job.pwdlen,saltbuf,serverfields::saltlen,serverfields::hashlen,encodedbuffer.data(),encodedbuffer.size());
            if (encoderes == ARGON2_OK) {
                hres.result = essential::hashresultstatus::ENCODEDONE;
                memcpy(hres.buf,encodedbuffer.data(),encodedbuffer.size());
                resqueue.push(std::move(hres));

                encodedbuffer.clear();
            }
            else {
                hres.result = essential::hashresultstatus::ENCODEFAILED;
                resqueue.push(std::move(hres));
            }
        }

        else {
            int rs = argon2_verify(job.hashbuf,job.pwdbuf,job.pwdlen,Argon2_id);
            if (rs == ARGON2_OK) {
                hres.result = essential::hashresultstatus::VERIFYDONE;
            }
            else {
                hres.result = essential::hashresultstatus::VERIFYFAIL;
            }
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
