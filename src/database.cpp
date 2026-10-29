#include "core/database.h"
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include "core/log.h"
#include <cstring>
#include "server/corestructures.h"
#include "server/serversettings.h"
#include "proto/objects.h"
#include "sys/eventfd.h"

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

template <typename T>
dbresultentry dbresinit(T& entry) {
    dbresultentry d;
    d.connid = entry.connid;
    d.accountid = entry.accountid;
    d.resultjob = entry.entrytype;
    return d;
}

struct readfunctor {
    database& db;

    //the overloads accept a type to distinguish cases where different job types use the same params
    dbresultentry operator()(readverifyparams& params,dbreadentry& r) {
        SERVERASSERT(r.entrytype == dbjobtype::READLOGINVERIFY,"");
        sqlite3_bind_text(db.getloginverify.s,db.getloginverify.bindusernameindex,params.username.buf,static_cast<int>(params.username.length),SQLITE_STATIC);
        
        dbresultentry res = dbresinit(r);
        resultverifyparams p;

        int status = sqlite3_step(db.getloginverify.s);

        //found a matching row
        if (status == SQLITE_ROW) {
            res.status = dbresultstatus::SUCCESS;

            uint64_t accid = static_cast<uint64_t>(sqlite3_column_int64(db.getloginverify.s,db.getloginverify.accidcolumnindex));
            res.accountid = accid;
            
            int hashbytecount = sqlite3_column_bytes(db.getloginverify.s,db.getloginverify.hashcolumnindex);
            SERVERASSERT(static_cast<size_t>(hashbytecount) == db.hashencodedlength, "DATABASE: Stored hash length does not correspond to the hashpool length field");
            
            p.hash.fill(reinterpret_cast<const char *>(sqlite3_column_text(db.getloginverify.s,db.getloginverify.hashcolumnindex)),static_cast<size_t>(hashbytecount));
            res.params.emplace<resultverifyparams>(p);
        }

        else if (status == SQLITE_DONE) res.status = dbresultstatus::READNOTFOUND;

        else {
            res.status = dbresultstatus::FAILEDFATAL;
            db.killdb();
            db.dbinterr(status,"DBREAD : loginverification");
        }

        sqlite3_reset(db.getloginverify.s);
        sqlite3_clear_bindings(db.getloginverify.s);
        return res;
    }
    dbresultentry operator()(readusercacheparams& params,dbreadentry& r) {
        SERVERASSERT(r.entrytype == dbjobtype::READUSERCACHE,"");
        sqlite3_bind_int64(db.getusercache.s,db.getusercache.bindaccidindex,static_cast<sqlite3_int64>(r.accountid));
        int status = sqlite3_step(db.getusercache.s);

        dbresultentry res = dbresinit(r);

        if (status == SQLITE_ROW) {
            res.status = dbresultstatus::SUCCESS;
            resultgetcacheparams cacheparams;

            int emailbytecount = sqlite3_column_bytes(db.getusercache.s,db.getusercache.emailcolumnindex);
            int usernamebytecount = sqlite3_column_bytes(db.getusercache.s,db.getusercache.usernamecolumnindex);

            cacheparams.email.fill(reinterpret_cast<const char *>(sqlite3_column_text(db.getusercache.s,db.getusercache.emailcolumnindex)),static_cast<uint32_t>(emailbytecount));
            cacheparams.username.fill(reinterpret_cast<const char *>(sqlite3_column_text(db.getusercache.s,db.getusercache.usernamecolumnindex)),static_cast<uint32_t>(usernamebytecount));
            cacheparams.emblems = static_cast<uint64_t>(sqlite3_column_int64(db.getusercache.s,db.getusercache.emblemscolumnindex));
            
            res.params.emplace<resultgetcacheparams>(cacheparams);
        }
        else if (status == SQLITE_DONE) {
            res.status = dbresultstatus::READNOTFOUND;
        }
        else {
            res.status = dbresultstatus::FAILEDFATAL;
            db.killdb();
            db.dbinterr(status,"DBREAD: fetchusercache");
        }

        sqlite3_reset(db.getusercache.s);
        sqlite3_clear_bindings(db.getusercache.s);

        return res;
    }
    dbresultentry operator()(readcheckuserparams& params,dbreadentry& r) {
        SERVERASSERT(r.entrytype == dbjobtype::READCHECKIFUSEREXISTS,"");
        sqlite3_bind_text(db.checkuser.s,db.checkuser.bindusernameindex,params.username.buf,static_cast<int>(params.username.length),SQLITE_STATIC);
        int status = sqlite3_step(db.checkuser.s);

        dbresultentry res = dbresinit(r);

        if (status == SQLITE_ROW) {
            res.status = dbresultstatus::SUCCESS;
        }
        else if (status == SQLITE_DONE) {
            res.status = dbresultstatus::READNOTFOUND;
        }
        else {
            res.status = dbresultstatus::FAILEDFATAL;
            db.killdb();
            db.dbinterr(status,"DBREAD : checkuserexistence");
        }

        sqlite3_reset(db.checkuser.s);
        sqlite3_clear_bindings(db.checkuser.s);

        return res;
    }
};

struct writefunctor {
    database& db;
    dbresultentry operator()(writeupdatecacheparams& params,dbwriteentry& w) {
        SERVERASSERT(w.entrytype == dbjobtype::WRITEUSERCACHE,"");

        sqlite3_bind_int64(db.updatecache.s,db.updatecache.bindemblemsindex,static_cast<sqlite3_int64>(params.emblems));
        sqlite3_bind_int64(db.updatecache.s,db.updatecache.bindaccidindex,static_cast<sqlite3_int64>(w.accountid));
        int status = sqlite3_step(db.updatecache.s);

        dbresultentry res = dbresinit(w);

        if (status == SQLITE_DONE) {
            res.status = dbresultstatus::SUCCESS;
        }   
        else {
            res.status = dbresultstatus::FAILEDFATAL;
            db.killdb();
            db.dbinterr(status,"DBWRITE : updatecache");
        }

        sqlite3_reset(db.updatecache.s);
        sqlite3_clear_bindings(db.updatecache.s);
        return res;
    }
    dbresultentry operator()(writenewuserparams& params,dbwriteentry& w) {
        SERVERASSERT(w.entrytype == dbjobtype::WRITENEWUSER,"");

        sqlite3_bind_text(db.putnewuser.s,db.putnewuser.bindusernameindex,params.username.buf,static_cast<int>(params.username.length),SQLITE_STATIC);
        sqlite3_bind_text(db.putnewuser.s,db.putnewuser.bindhashindex,params.hash.buf,static_cast<int>(params.hash.length),SQLITE_STATIC);
        sqlite3_bind_text(db.putnewuser.s,db.putnewuser.bindemailindex,params.email.buf,static_cast<int>(params.email.length),SQLITE_STATIC);

        dbresultentry res = dbresinit(w);

        int status = sqlite3_step(db.putnewuser.s);
        sqlite3_reset(db.putnewuser.s);
        sqlite3_clear_bindings(db.putnewuser.s);

        if (status == SQLITE_DONE) {
            //retrieve the inserted accountid
            res.accountid = static_cast<uint64_t>(sqlite3_last_insert_rowid(db.getdb()));
            res.status = dbresultstatus::SUCCESS;
        }
        else if (status == SQLITE_CONSTRAINT_UNIQUE) {
            //somebody with same chosen username hashed first
            res.status = dbresultstatus::WRITEFAILEDUNIQUE;
        }
        else {
            res.status = dbresultstatus::FAILEDFATAL;
            db.killdb();  
            db.dbinterr(status,"DBWRITE : newuser");
        }

        sqlite3_reset(db.putnewuser.s);
        sqlite3_clear_bindings(db.putnewuser.s);
        return res;
    }

};

void database::signalmainthread() {
    uint64_t val = 1;
    ssize_t evwrite = eventfd_write(resulteventfd,val);
    if (evwrite == -1) {
        int errnum = errno;
        LOGERRORNUM(errnum,"DATABASE: eventfd write err!");
        SERVERFAIL("DATABASE: Eventfd write failure");
    }

    SERVERASSERT(static_cast<size_t>(evwrite) == sizeof val,"");
}

void database::sendshutdownsignal() {
    dbresultentry shutdownres;
    shutdownres.connid = 0;
    shutdownres.status = dbresultstatus::FAILEDFATAL;

    std::lock_guard<std::mutex> lck(resultmtx);
    resultqueue.push(std::move(shutdownres));

    signalmainthread();
}

void database::work() {
    std::queue<dbreadentry> localreads;
    std::queue<dbwriteentry> localwrites;
    std::vector<dbresultentry> localresults;
    readfunctor readvisitor{.db = *this};
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
                [&](auto& params) -> dbresultentry { return readvisitor(params,r); }, r.params
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

        if (totalreads > 0) {
            //atleast one result was pushed
            signalmainthread();
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
                    [&](auto& params) -> dbresultentry { return writevisitor(params,w); }, w.params
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

            SERVERASSERT(stopdatabase == false, "DATABASE: Execution continued despite database stop signal being set");
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
        if (localresults.size() > totalreads) {
            //atleast one write job result was pushed
            signalmainthread();
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

bool database::init(size_t encodelength, int efd) {
    hashencodedlength = encodelength;
    resulteventfd = efd;
    SERVERASSERT(hashencodedlength > 0, "DATABASE: encodelength parameter is invalid");
    SERVERASSERT(resulteventfd >= 0, "DATABASE: eventfd parameter is invalid");

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
    
    const char * querygetuserinfo = "SELECT username,email,emblems FROM users WHERE account_id = :accountid;";
    if ((status = preparestatement(db,&getusercache.s,querygetuserinfo,"INIT : prepgetcache")) == false) return false;

    const char * insertnewuser = "INSERT INTO users (username,phash,email) VALUES (:username,:phash,:email);";
    if ((status = preparestatement(db,&putnewuser.s,insertnewuser,"INIT : prepsignupinsert")) == false) return false;

    const char * querycheckuser = "SELECT 1 FROM users WHERE username = :username LIMIT 1;";
    if ((status = preparestatement(db,&checkuser.s,querycheckuser,"INIT : prepcheckuserexistence")) == false) return false;

    const char * updateusercachedata = "UPDATE users SET emblems = :emblems WHERE account_id = :accountid;";
    if ((status = preparestatement(db,&updatecache.s,updateusercachedata,"INIT : prepusercacheupdate")) == false) return false;

    //setting bind parameter indices
    SERVERASSERT((getloginverify.bindusernameindex = sqlite3_bind_parameter_index(getloginverify.s,":username")) != 0,"");
    SERVERASSERT((putnewuser.bindusernameindex = sqlite3_bind_parameter_index(putnewuser.s,":username")) != 0,"");
    SERVERASSERT((putnewuser.bindhashindex = sqlite3_bind_parameter_index(putnewuser.s,":phash")) != 0,"");
    SERVERASSERT((putnewuser.bindemailindex = sqlite3_bind_parameter_index(putnewuser.s,":email")) != 0,"");
    SERVERASSERT((checkuser.bindusernameindex = sqlite3_bind_parameter_index(checkuser.s,":username")) != 0,"");
    SERVERASSERT((getusercache.bindaccidindex = sqlite3_bind_parameter_index(getusercache.s,":accountid")) != 0,"");
    SERVERASSERT((updatecache.bindemblemsindex = sqlite3_bind_parameter_index(updatecache.s,":emblems")) != 0,"");
    SERVERASSERT((updatecache.bindaccidindex = sqlite3_bind_parameter_index(updatecache.s,":accountid")) != 0,"");

    //setting up column indices
    SERVERASSERT((getloginverify.accidcolumnindex = getcolindex(getloginverify.s,"account_id")) != -1,"");
    SERVERASSERT((getloginverify.hashcolumnindex = getcolindex(getloginverify.s,"phash")) != -1,"");
    SERVERASSERT((getusercache.usernamecolumnindex = getcolindex(getusercache.s,"username")) != -1,"");
    SERVERASSERT((getusercache.emailcolumnindex = getcolindex(getusercache.s,"email")) != -1,"");
    SERVERASSERT((getusercache.emblemscolumnindex = getcolindex(getusercache.s,"emblems")) != -1,"");

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

    readjob.params.emplace<0>(params);

    SERVERASSERT(readjob.accountid == 0,"DATABASE: accountid already set for preauth job"); //Pre auth jobs cannot have accid set
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
    
    SERVERASSERT(writejob.accountid == 0,"DATABASE: accountid already set for preauth job");
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

    SERVERASSERT(readjob.accountid == 0,"DATABASE: accountid already set for preauth job");
    std::lock_guard<std::mutex> lck(mtx);
    readqueue.push(std::move(readjob));
}

void database::submitfetchusercachejob(uint64_t connid,uint64_t accid) {
    dbreadentry readjob;
    readjob.connid = connid;
    readjob.accountid = accid;
    readjob.entrytype = dbjobtype::READUSERCACHE;

    readusercacheparams params;
    readjob.params.emplace<readusercacheparams>(params);

    SERVERASSERT(readjob.accountid != 0,"DATABASE: accountid not set for postauth job"); //post auth job must have an accid  
    std::lock_guard<std::mutex> lck(mtx);
    readqueue.push(std::move(readjob));
 
}

void database::submitupdateusercachejob(uint64_t connid,uint64_t accountid,fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxemailsize + 1>& email,uint64_t emblems) {
    dbwriteentry writejob;
    writejob.connid = connid;
    writejob.accountid = accountid;
    writejob.entrytype = dbjobtype::WRITEUSERCACHE;
    
    writeupdatecacheparams params;
    params.emblems = emblems;
    writejob.params.emplace<writeupdatecacheparams>(params);

    SERVERASSERT(writejob.accountid != 0, "DATABASE: accountid not set for postauth job");
    std::lock_guard<std::mutex> lck(mtx);
    writequeue.push(std::move(writejob));
}

database::~database() {
    stop();
}

void database::killdb() {
    stopdatabase = true;
}

void database::getresults(std::queue<dbresultentry>& localqueue) {
    std::lock_guard<std::mutex> lck(resultmtx);
    localqueue.swap(resultqueue);
} 

sqlite3 * database::getdb() {
    return db;
}

void database::stop() {
    if (killthread == true) true;
    {
        std::lock_guard<std::mutex> lck(mtx);
        killthread = true;
    }
    cv.notify_all();
    if (dbthread.joinable()) dbthread.join();
}
