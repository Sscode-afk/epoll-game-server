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
    LOGERROR("Sqlite error on %s : %s",stage,sqlite3_errstr(errcode));
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
        sqlite3_bind_text(db.stmtget_loginverification,1,params.username.buf,static_cast<int>(params.username.length),SQLITE_STATIC);
        
        dbresultentry res;
        resultverifyparams resparam;
    
        int status = sqlite3_step(db.stmtget_loginverification);

        //found a matching row
        if (status == SQLITE_ROW) {
            resparam.success = true;

            uint64_t accid = static_cast<uint64_t>(sqlite3_column_int64(db.stmtget_loginverification,VERIFYACCOUNTIDCOLUMN));
            int hashbytecount = sqlite3_column_bytes(db.stmtget_loginverification,VERIFYHASHCOLUMN);

            fixedcharbuffer<serverfields::hashresultbufsize> hashbuf;
            SERVERASSERT(static_cast<size_t>(hashbytecount) == hashpoolref.encodedlen - 1);
            hashbuf.fill(reinterpret_cast<const char *>(sqlite3_column_text(db.stmtget_loginverification,VERIFYHASHCOLUMN)),static_cast<size_t>(hashbytecount));

            essential::hashentry hashjob{ .connid = connid, .accountid = accid, .jtype = essential::hashpooljobtype::VERIFY, .password = params.password, .hash = hashbuf};
            hashpoolref.submit(std::move(hashjob));

            explicit_bzero(params.password.buf,params.password.length); //clear the password
        }

        else resparam.success = false;

        res.connid = connid;
        res.resultjob = type;
        res.params.emplace<1>(resparam);
        return res;
    }
    dbresultentry operator()(readusercacheparams& params,dbjobtype type,uint64_t connid) {

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
};

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
            localresults.emplace_back( 
                std::visit(
                    [&](auto& params) -> dbresultentry { return readvisitor(params,r.entrytype,r.connid); }, r.params
                )
            );
            localreads.pop();
        }

        {
            std::lock_guard<std::mutex> lck(resultmtx);
            for (size_t i = 0; i < totalreads; i++) {
                resultqueue.push(std::move(localresults[i]));
            }
        }

        while (!localwrites.empty()) {
            dbwriteentry& w = localwrites.front();
            localresults.emplace_back(
                std::visit(
                    [&](auto& params) -> dbresultentry { return writevisitor(params,w.entrytype,w.connid); }, w.params
                )  
            );
            localwrites.pop();
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

bool database::init() {
    std::filesystem::path dbpath = "database/server.db";
    if (!std::filesystem::is_directory(dbpath.parent_path())) {
        LOGERROR("Database directory (%s) does not exist!",std::filesystem::absolute(dbpath.parent_path()).c_str());
        return false;
    }

    int status = sqlite3_open_v2(dbpath.c_str(),&db,SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_CREATE,nullptr);

    if (status != SQLITE_OK) {
        dbinterr(status,"INIT : open");
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
        " emblems INTEGER NOT NULL"
        ");";
    
    if ((status = executequery(schema,"INIT : usersschema")) == false) return false;

    //preparing the queries
    const char * queryloginverification = "SELECT account_id,username,phash FROM users WHERE username = ?1;";
    if ((status = preparestatement(db,&stmtget_loginverification,queryloginverification,"INIT : preploginverify")) == false) return false;
    
    const char * querygetuserinfo = "SELECT * FROM users WHERE account_id = ?1;";
    if ((status = preparestatement(db,&stmtget_usercachedata,querygetuserinfo,"INFO : prepgetcache")) == false) return false;

    const char * insertnewuser = "INSERT INTO users (username,phash) VALUES (?1,?2);";
    if ((status = preparestatement(db,&stmtput_newuser,insertnewuser,"INIT : prepsignupinsert")) == false) return false;

    //checks if the columns and the associated indexes match
    SERVERASSERT(std::string_view(sqlite3_column_name(stmtget_loginverification,VERIFYACCOUNTIDCOLUMN)) == "account_id");
    SERVERASSERT(std::string_view(sqlite3_column_name(stmtget_loginverification,VERIFYHASHCOLUMN)) == "phash");

    if (dbthread.joinable()) {
        dbthread = std::thread(&database::work,this);
    }

    LOGINFO("Database init successful!");
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

database::database(hashpool& ref) : hp(ref) {}

database::~database() {
    {
        std::lock_guard<std::mutex> lck(mtx);
        killthread = true;
    }
    cv.notify_all();
    if (dbthread.joinable()) dbthread.join();
}
