#include "core/database.h"
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include "core/log.h"
#include <cstring>
#include "server/corestructures.h"
#include "server/serversettings.h"
#include "proto/objects.h"

void database::dbinterr(int errcode, const char * stage) {
    LOGERROR("DATABASE: Sqlite error on %s : %s",stage,sqlite3_errstr(errcode));
}

bool database::executequery(const char * query,const char * stage) {
    int status = sqlite3_exec(db,query,nullptr,nullptr,nullptr);
    if (status != SQLITE_OK) {
        dbinterr(status,stage);
        return false;
    }
    return true;
}

bool database::preparestatement(sqlite3* dbpointer,sqlite3_stmt** stmt, const char * query, const char * stage) {
    int status = sqlite3_prepare_v2(dbpointer,query,-1,stmt,nullptr);
    if (status != SQLITE_OK) {
        dbinterr(status,stage);
        return false;
    }
    return true;
}

struct readfunctor {
    database& db;
    hashpool& hashpoolref;

    //the overloads accept a type to distinguish cases where different job types use the same params
    dbresultentry operator()(readverifyparams& params,dbjobtype type,uint64_t connid) {
        SERVERASSERT(type == dbjobtype::READLOGINVERIFY);
        sqlite3_bind_text(db.getloginverify.s,db.getloginverify.bindusernameindex,params.username.buf,static_cast<int>(params.username.length),SQLITE_STATIC);
        
        dbresultentry res;

        int status = sqlite3_step(db.getloginverify.s);

        //found a matching row
        if (status == SQLITE_ROW) {
            res.status = dbresultstatus::SUCCESS;

            uint64_t accid = static_cast<uint64_t>(sqlite3_column_int64(db.getloginverify.s,db.getloginverify.accidcolumnindex));
            int hashbytecount = sqlite3_column_bytes(db.getloginverify.s,db.getloginverify.hashcolumnindex);

            fixedcharbuffer<serverfields::hashresultbufsize> hashbuf;
            SERVERASSERT(static_cast<size_t>(hashbytecount) == hashpoolref.encodedlen - 1);
            hashbuf.fill(reinterpret_cast<const char *>(sqlite3_column_text(db.getloginverify.s,db.getloginverify.hashcolumnindex)),static_cast<size_t>(hashbytecount));

            essential::hashentry hashjob{ .connid = connid, .accountid = accid, .jtype = essential::hashpooljobtype::VERIFY, .password = params.password, .hash = hashbuf};
            hashpoolref.submit(std::move(hashjob));

            explicit_bzero(params.password.buf,params.password.length); //clear the password
        }

        else if (status == SQLITE_DONE) res.status = dbresultstatus::READNOTFOUND;

        else {
            res.status = dbresultstatus::FAILEDFATAL;
            db.killdb();
            db.dbinterr(status,"DBREAD : loginverification");
        }

        sqlite3_reset(db.getloginverify.s);
        sqlite3_clear_bindings(db.getloginverify.s);

        res.connid = connid;
        res.resultjob = type;
        return res;
    }
    dbresultentry operator()(readusercacheparams& params,dbjobtype type,uint64_t connid) {
        
    }
    dbresultentry operator()(readcheckuserparams& params,dbjobtype type,uint64_t connid) {
        SERVERASSERT(type == dbjobtype::READCHECKIFUSEREXISTS);
        sqlite3_bind_text(db.checkuser.s,db.checkuser.bindusernameindex,params.username.buf,static_cast<int>(params.username.length),SQLITE_STATIC);
        int rs = sqlite3_step(db.checkuser.s);

        dbresultentry res;

        if (rs == SQLITE_ROW) {
            res.status = dbresultstatus::SUCCESS;
        }
        else res.status = dbresultstatus::READNOTFOUND;

        res.connid = connid;
        res.resultjob = type;
        return res;
    }
};

struct writefunctor {
    database& db;
    dbresultentry operator()(writeupdatecacheparams& params,dbjobtype type,uint64_t connid) {
        dbresultentry res;

        res.connid = connid;
        res.resultjob = type;
        return res;
    }
    dbresultentry operator()(writenewuserparams& params,dbjobtype type,uint64_t connid) {
        SERVERASSERT(type == dbjobtype::WRITENEWUSER);

        sqlite3_bind_text(db.putnewuser.s,db.putnewuser.bindusernameindex,params.username.buf,static_cast<int>(params.username.length),SQLITE_STATIC);
        sqlite3_bind_text(db.putnewuser.s,db.putnewuser.bindhashindex,params.hash.buf,static_cast<int>(params.hash.length),SQLITE_STATIC);
        sqlite3_bind_text(db.putnewuser.s,db.putnewuser.bindemailindex,params.email.buf,static_cast<int>(params.email.length),SQLITE_STATIC);

        dbresultentry res;

        int rs = sqlite3_step(db.putnewuser.s);
        sqlite3_reset(db.putnewuser.s);
        sqlite3_clear_bindings(db.putnewuser.s);

        if (rs == SQLITE_DONE) {
            res.status = dbresultstatus::SUCCESS;
        }
        else if (rs == SQLITE_CONSTRAINT_UNIQUE) {
            //somebody with same chosen username hashed first
            res.status = dbresultstatus::WRITEFAILEDUNIQUE;
        }
        else {
            res.status = dbresultstatus::FAILEDFATAL;
            db.killdb();  
            db.dbinterr(rs,"DBWRITE : newuser");
        }

        res.connid = connid;
        res.resultjob = type;

        return res;
    }
};

void database::sendshutdownsignal() {
    dbresultentry shutdownres;
    shutdownres.connid = 0;
    shutdownres.status = dbresultstatus::FAILEDFATAL;

    std::lock_guard<std::mutex> lck(resultmtx);
    resultqueue.push(std::move(shutdownres));
}

void database::work() {
    std::queue<dbreadentry> localreads;
    std::queue<dbwriteentry> localwrites;
    std::vector<dbresultentry> localresults;
    readfunctor readvisitor{.db = *this,.hashpoolref = hp};
    writefunctor writevisitor{.db = *this};
    while (true) {
        {
            std::unique_lock<std::mutex> lck(mtx);
            cv.wait(lck,[&]()->bool { return (killthread || readqueue.empty() == false || writequeue.empty() == false); });
            if (killthread && readqueue.empty() && writequeue.empty()) return;
            localreads.swap(readqueue);
            localwrites.swap(writequeue);
        }

        localresults.reserve(localreads.size() + localwrites.size());
        size_t totalreads = localreads.size(); //pop brings reads down to 0, storing it.
        while (!localreads.empty()) {
            dbreadentry& r = localreads.front();
            dbresultentry res = std::visit(
                [&](auto& params) -> dbresultentry { return readvisitor(params,r.entrytype,r.connid); }, r.params
            );

            if (stopdatabase) {
                sendshutdownsignal();
                return;
            }

            localresults.push_back(std::move(res));
            localreads.pop();
        }

        {
            std::lock_guard<std::mutex> lck(resultmtx);
            for (size_t i = 0; i < totalreads; i++) {
                resultqueue.push(std::move(localresults[i]));
            }
        }

        if (!localwrites.empty()) {
            //open a SQLITE write transaction
            int transaction = sqlite3_exec(db,"BEGIN IMMEDIATE;",nullptr,nullptr,nullptr);
            if (transaction != SQLITE_OK) {
                dbinterr(transaction,"WORKER : begintransaction");
                sendshutdownsignal();
                return;
            }

            while (!localwrites.empty()) {
                dbwriteentry& w = localwrites.front();
                dbresultentry res = std::visit (
                    [&](auto& params) -> dbresultentry { return writevisitor(params,w.entrytype,w.connid); }, w.params
                );

                if (stopdatabase) {
                    //rollback and pack the shutdown message
                    sqlite3_exec(db,"ROLLBACK;",nullptr,nullptr,nullptr);
                    sendshutdownsignal();
                    return;
                }

                localresults.push_back(std::move(res));
                localwrites.pop();
            }

            SERVERASSERT(stopdatabase == false);
            int rs = sqlite3_exec(db,"COMMIT;",nullptr,nullptr,nullptr);
            if (rs != SQLITE_OK) {
                //commit failures shut down the server
                //if in autocommit mode, then do not explicitly ROLLBACK
                if (!sqlite3_get_autocommit(db)) sqlite3_exec(db,"ROLLBACK;",nullptr,nullptr,nullptr);
                dbinterr(rs,"WORKER : writebatchcommit");

                sendshutdownsignal();
                return;
            }
        }

        {
            std::lock_guard<std::mutex> lck(resultmtx);
            for (size_t i = totalreads; i < localresults.size(); i++) {
                resultqueue.push(std::move(localresults[i]));
            }
        }

        localresults.clear();
    }
}

int getcolindex(sqlite3_stmt * s, const char * columnname) {
    int n = sqlite3_column_count(s);
    for (int i = 0; i < n; i++) {
        if (std::string_view(sqlite3_column_name(s,i)) == columnname) return i;
    }
    return -1;
}

bool database::init() {
    std::filesystem::path dbpath = "database/server.db";
    if (!std::filesystem::is_directory(dbpath.parent_path())) {
        LOGERROR("DATABASE: Database directory (%s) does not exist!",std::filesystem::absolute(dbpath.parent_path()).c_str());
        return false;
    }

    int status = sqlite3_open_v2(dbpath.c_str(),&db,SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_CREATE,nullptr);

    if (status != SQLITE_OK) {
        dbinterr(status,"INIT : open");
        return false;
    }

    if ((status = sqlite3_extended_result_codes(db,1))!=SQLITE_OK) {
        dbinterr(status,"INIT : setextendedcodes");
        return false;
    }

    //setting up sqlite behavior using pragmas
    if ((status = executequery("PRAGMA journal_mode = WAL;","INIT : setWAL")) == false) return false;
    if ((status = executequery("PRAGMA synchronous = NORMAL;","INIT : syncmode")) == false) return false;
    if ((status = executequery("PRAGMA busy_timeout = 5000;","INIT : busytimeout")) == false) return false;
    
    const char * schema =
        "CREATE TABLE IF NOT EXISTS users ("
        " account_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT UNIQUE NOT NULL,"
        " phash TEXT NOT NULL,"
        " email TEXT NOT NULL,"
        " emblems INTEGER NOT NULL DEFAULT 0"
        ");";
    
    if ((status = executequery(schema,"INIT : usersschema")) == false) return false;

    //preparing the queries
    const char * queryloginverification = "SELECT account_id,username,phash FROM users WHERE username = :username;";
    if ((status = preparestatement(db,&getloginverify.s,queryloginverification,"INIT : preploginverify")) == false) return false;
    
    const char * querygetuserinfo = "SELECT * FROM users WHERE account_id = ?1;";
    if ((status = preparestatement(db,&stmtget_usercachedata,querygetuserinfo,"INIT : prepgetcache")) == false) return false;

    const char * insertnewuser = "INSERT INTO users (username,phash,email) VALUES (:username,:phash,:email);";
    if ((status = preparestatement(db,&putnewuser.s,insertnewuser,"INIT : prepsignupinsert")) == false) return false;

    const char * checkuserquery = "SELECT 1 FROM users WHERE username = :username LIMIT 1;";
    if ((status = preparestatement(db,&checkuser.s,checkuserquery,"INIT : prepcheckuserexistence")) == false) return false;
    
    //setting bind parameter indices
    SERVERASSERT((getloginverify.bindusernameindex = sqlite3_bind_parameter_index(getloginverify.s,":username")) != 0);
    SERVERASSERT((putnewuser.bindusernameindex = sqlite3_bind_parameter_index(putnewuser.s,":username")) != 0);
    SERVERASSERT((putnewuser.bindhashindex = sqlite3_bind_parameter_index(putnewuser.s,":phash")) != 0);
    SERVERASSERT((putnewuser.bindemailindex = sqlite3_bind_parameter_index(putnewuser.s,":email")) != 0);
    SERVERASSERT((checkuser.bindusernameindex = sqlite3_bind_parameter_index(checkuser.s,":username")) != 0);

    //setting up column indices
    SERVERASSERT((getloginverify.accidcolumnindex = getcolindex(getloginverify.s,"account_id")) != -1);
    SERVERASSERT((getloginverify.hashcolumnindex = getcolindex(getloginverify.s,"phash")) != -1);

    if (dbthread.joinable()) {
        dbthread = std::thread(&database::work,this);
    }

    LOGINFO("DATABASE: Database init successful!");
    return true;
}

void database::submitloginauthjob(uint64_t connid,fixedcharbuffer<network::maxcredentialsize+1>& username,fixedcharbuffer<network::maxcredentialsize+1>& password) {
    dbreadentry readjob;
    readjob.connid = connid;
    readjob.entrytype = dbjobtype::READLOGINVERIFY;
    readverifyparams params;
    params.username = username;
    params.password = password;

    readjob.params.emplace<0>(params);

    std::lock_guard<std::mutex> lck(mtx);
    readqueue.push(std::move(readjob));
}

void database::submitsignupjob(uint64_t connid,fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxemailsize + 1>& email, fixedcharbuffer<serverfields::hashresultbufsize>& hash) {
    dbwriteentry writejob;
    writejob.connid = connid;
    writejob.entrytype = dbjobtype::WRITENEWUSER;

    writenewuserparams params;
    params.email = email;
    params.username = username;
    params.hash = hash;

    writejob.params.emplace<writenewuserparams>(params);
    
    std::lock_guard<std::mutex> lck(mtx);
    writequeue.push(std::move(writejob));
}

void database::submituserexistencejob(uint64_t connid, fixedcharbuffer<network::maxcredentialsize +1>& username) {
    dbreadentry readjob;
    readjob.connid = connid;
    readjob.entrytype = dbjobtype::READCHECKIFUSEREXISTS;

    readcheckuserparams params;
    params.username = username;
    readjob.params.emplace<readcheckuserparams>(params);

    std::lock_guard<std::mutex> lck(mtx);
    readqueue.push(std::move(readjob));
}

database::database(hashpool& ref) : hp(ref) {}

database::~database() {
    {
        std::lock_guard<std::mutex> lck(mtx);
        killthread = true;
    }
    cv.notify_all();
    if (dbthread.joinable()) dbthread.join();
}

void database::killdb() {
    stopdatabase = true;
}

void database::getresults(std::queue<dbresultentry>& localqueue) {
    std::lock_guard<std::mutex> lck(resultmtx);
    localqueue.swap(resultqueue);
}
