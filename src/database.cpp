#include "core/database.h"
#include <sqlite3.h>
#include <filesystem>
#include <string>
#include "core/log.h"

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

    LOGINFO("Database init successful!");
    return true;
}

