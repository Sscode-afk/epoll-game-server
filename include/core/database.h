#include <sqlite3.h>
#include <queue>
#include "proto/settings.h"
#include <variant>
#include <mutex>
#include <condition_variable>

enum class dbjobtype {READLOGINVERIFY,READUSERCACHE,WRITEUSERCACHE};
enum class dbresultstatus {};

//read params
struct readverifyparams {
    char username[network::maxcredentialsize + 1];
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
struct resultcacheparams {

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
    bool success;
    std::variant<resultcacheparams> params;
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

        void work();
    public:
        sqlite3_stmt* stmtget_loginverification = nullptr;
        sqlite3_stmt* stmtget_usercachedata = nullptr;

        bool init();
        void dbinterr(int errcode, const char * stage);

};  
