#pragma once
#include <sqlite3.h>
#include <queue>
#include "proto/settings.h"
#include "proto/objects.h"
#include <variant>
#include <mutex>
#include <condition_variable>
#include <thread>
#include "core/threadpool.h"

enum class dbjobtype {READLOGINVERIFY,READUSERCACHE,WRITEUSERCACHE};
enum class dbresultstatus {};

//read params
struct readverifyparams {
    fixedcharbuffer<network::maxcredentialsize + 1> username;
    fixedcharbuffer<network::maxcredentialsize + 1> password;
};

struct readusercacheparams {
    uint64_t useraccountid;
};

//write params
struct writeupdatecacheparams {
    uint64_t accountid;
    char username[network::maxcredentialsize +1];
    uint64_t emblems;
};

//result params
struct resultonlystatus {
    bool success;
};
struct resultcacheparams {
    bool success;
};
struct resultverifyparams {
    bool success;
};

//entries
struct dbreadentry {
    uint64_t connid;
    dbjobtype entrytype;
    std::variant<readverifyparams,readusercacheparams> params;
};

struct dbwriteentry {
    uint64_t connid;
    dbjobtype entrytype;
    std::variant<writeupdatecacheparams> params;
};

struct dbresultentry {
    uint64_t connid;
    dbjobtype resultjob;
    std::variant<resultonlystatus,resultverifyparams,resultcacheparams> params;
};

constexpr int VERIFYACCOUNTIDCOLUMN = 0;
constexpr int VERIFYHASHCOLUMN = 2;

struct database {
    private:
        hashpool& hp;
        sqlite3* db = nullptr;
        bool executequery(const char * query,const char * stage);
        bool preparestatement(sqlite3* database,sqlite3_stmt** stmt, const char * query, const char * stage);
    
        std::queue<dbreadentry> readqueue;
        std::queue<dbwriteentry> writequeue;
        std::queue<dbresultentry> resultqueue;

        std::mutex mtx;
        std::condition_variable cv;

        std::mutex resultmtx;

        void work();
        std::thread dbthread;

        bool killthread;
    public:
        sqlite3_stmt* stmtget_loginverification = nullptr;
        sqlite3_stmt* stmtget_usercachedata = nullptr;
        sqlite3_stmt* stmtput_newuser = nullptr;

        bool init();
        void dbinterr(int errcode, const char * stage);
        void submitloginauthjob(uint64_t connid,fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxcredentialsize + 1>& password);
        bool submitsignupjob(const char * username, const char * password,const char * email);

        database(hashpool& ref);
        ~database();
};  