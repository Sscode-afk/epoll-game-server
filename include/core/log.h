#pragma once
#include <cstdarg>
#include <stdlib.h>

#define LOGERROR(...) do { if(loglevel::ERROR >= lglvl) { log_write(loglevel::ERROR,__FILE__,__LINE__,__VA_ARGS__); } } while(0)
#define LOGERRORNUM(errnum,...) do { if(loglevel::ERROR >= lglvl) { log_write(loglevel::ERROR,(errnum),__FILE__,__LINE__,__VA_ARGS__); } } while(0)
#define LOGWARNING(...) do { if(loglevel::WARNING >= lglvl) { log_write(loglevel::WARNING,__FILE__,__LINE__,__VA_ARGS__); } } while(0)
#define LOGWARNINGNUM(errnum,...) do { if(loglevel::WARNING >= lglvl) { log_write(loglevel::WARNING,(errnum),__FILE__,__LINE__,__VA_ARGS__); } } while(0)
#define LOGINFO(...) do { if(loglevel::INFO >= lglvl) { log_write(loglevel::INFO,__FILE__,__LINE__,__VA_ARGS__); } } while(0) 
#define LOGDEBUG(...) do { if(loglevel::DEBUG >= lglvl) { log_write(loglevel::DEBUG,__FILE__,__LINE__,__VA_ARGS__); } } while(0)

#define SERVERASSERT(cond) do { if(!(cond)) { LOGERROR("Invariant failed: %s",#cond); std::abort();}} while(0)

enum class loglevel {DEBUG = 0, INFO = 1, WARNING = 2, ERROR = 3};
inline loglevel lglvl = loglevel::DEBUG;

void setloglevel(loglevel lvl);
const char * getloglevel(loglevel lvl);

void vlog_write(loglevel lvl,int errnum,const char * file,int line,const char * fmt,va_list args);

__attribute__((format(printf,4,5)))
void log_write(loglevel lvl,const char * file,int line,const char * fmt,...);

__attribute__((format(printf,5,6)))
void log_write(loglevel lvl,int errnum,const char * file,int line,const char * fmt,...);