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

enum class dbjobtype {READLOGINVERIFY,READUSERCACHE,READCHECKIFUSEREXISTS,WRITEUSERCACHE,WRITENEWUSER};
enum class dbresultstatus {SUCCESS,READNOTFOUND,WRITEFAILEDUNIQUE,FAILEDFATAL};

//read params
struct readverifyparams {
    fixedcharbuffer<network::maxcredentialsize + 1> username;
};

struct readcheckuserparams {
    fixedcharbuffer<network::maxcredentialsize + 1> username;
};

struct readusercacheparams {}; //tag only to identify the type of statement to use, uses account id already present in the session bound read job

//write params
struct writeupdatecacheparams {
    uint64_t emblems;
};

struct writenewuserparams {
    fixedcharbuffer<network::maxcredentialsize + 1> username;
    fixedcharbuffer<network::maxemailsize + 1> email;
    fixedcharbuffer<serverfields::hashresultbufsize> hash;
};

//result params
struct resultgetcacheparams {
    fixedcharbuffer<network::maxcredentialsize + 1> username;
    fixedcharbuffer<network::maxemailsize + 1> email;
    uint64_t emblems;
};

struct resultverifyparams {
    fixedcharbuffer<serverfields::hashresultbufsize> hash;
};

//entries
struct dbreadentry {
    uint64_t connid;
    uint64_t accountid = 0;
    dbjobtype entrytype;
    std::variant<readverifyparams,readusercacheparams,readcheckuserparams> params;
};

struct dbwriteentry {
    uint64_t connid;
    uint64_t accountid = 0;
    dbjobtype entrytype;
    std::variant<writeupdatecacheparams,writenewuserparams> params;
};

struct dbresultentry {
    uint64_t connid;
    uint64_t accountid = 0; //0 indicates absence
    dbjobtype resultjob;
    dbresultstatus status;
    //with monostate, params is default constructible to indicate absence of a type at the moment
    std::variant<std::monostate,resultverifyparams,resultgetcacheparams> params;
};

//stmt
struct adduserstmt {
    sqlite3_stmt * s = nullptr;
    int bindusernameindex, bindhashindex, bindemailindex;
};

struct verifyloginstmt {
    sqlite3_stmt * s = nullptr;
    int bindusernameindex;
    int accidcolumnindex, hashcolumnindex;
};

struct userexistencestmt {
    sqlite3_stmt * s = nullptr;
    int bindusernameindex;
};

struct getuserdatastmt {
    sqlite3_stmt * s = nullptr;
    int bindaccidindex;
    int usernamecolumnindex,emailcolumnindex,emblemscolumnindex;
};

struct updateusercachestmt {
    sqlite3_stmt * s = nullptr;
    int bindemblemsindex,bindaccidindex;
};

struct database {
    private:
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
        bool stopdatabase = false;
        void sendshutdownsignal();

        bool killthread;
        int resulteventfd = -1;

        void signalmainthread();
    public:
        size_t hashencodedlength = 0;

        verifyloginstmt getloginverify;
        getuserdatastmt getusercache;
        adduserstmt putnewuser;
        userexistencestmt checkuser;
        updateusercachestmt updatecache;

        bool init(size_t encodelength,int reseventfd);
        void dbinterr(int errcode, const char * stage);
        void submitloginauthjob(uint64_t connid,fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxcredentialsize + 1>& password);
        void submitsignupjob(uint64_t connid,fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxemailsize + 1>& email, fixedcharbuffer<serverfields::hashresultbufsize>& hash);
        void submituserexistencejob(uint64_t connid, fixedcharbuffer<network::maxcredentialsize + 1>& username);
        void submitfetchusercachejob(uint64_t connid,uint64_t accountid);
        void submitupdateusercachejob(uint64_t connid,uint64_t accountid,fixedcharbuffer<network::maxcredentialsize + 1>& username,fixedcharbuffer<network::maxemailsize + 1>& email,uint64_t emblems);

        void killdb();
        void getresults(std::queue<dbresultentry>& localqueue);
        sqlite3 * getdb();
        ~database();
        void stop();
};  