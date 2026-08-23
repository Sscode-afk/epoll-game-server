#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "core/log.h"
#include <time.h>
#include <stdio.h>
#include <cstring>
#include <unistd.h>

void setloglevel(loglevel lvl) {
    lglvl = lvl;
}

const char * getloglevel(loglevel lvl) {
    switch(lvl) {
        case loglevel::ERROR : {
            return "ERROR";
        }
        case loglevel::WARNING : {
            return "WARNING";
        }
        case loglevel::INFO : {
            return "INFO";
        }
        case loglevel::DEBUG : {
            return "DEBUG";
        }
    }

    return "?";
}

struct appender {
    private:
        char buf[1024];
        size_t n = 0;
        size_t cap = sizeof buf - 1;

    public:
        size_t getcap() {
            return cap;
        }

        __attribute__((format(printf,2,3))) //2-3 because in a member function, 1st parameter becomes 'this'
        void append(const char * fmt,...) {
            if (n >= cap) return; //runs only if space left

            va_list args;
            va_start(args,fmt);

            int k = vsnprintf(buf + n,cap - n,fmt,args);
            va_end(args);

            if (k > 0) n += static_cast<size_t>(k);
        }

        void append_v(const char * fmt,va_list args) {
            if (n >= cap) return;

            int k = vsnprintf(buf+n,cap-n,fmt,args);

            if (k > 0) n+= static_cast<size_t>(k);
        }

        void done() {
            if (n>=cap) {
                n = cap;
                const char * ellipses = "...";
                size_t count = 3;

                if (cap < 3) count = cap;
                memcpy(buf + n - count,ellipses,count);
            }

            buf[n] = '\n';
            write(2,buf,n+1);
        }
};
void vlog_write(loglevel lvl,int errnum,const char * file,int line,const char * fmt,va_list args) {
    appender log;

    if (log.getcap() <=0) {
        fprintf(stderr,"Invalid buffer cap size: %zu",log.getcap());
        return;
    }

    time_t now = time(nullptr);
    tm tmbuf;

    localtime_r(&now,&tmbuf);

    //small time buffer
    char timechar[32];
    size_t z = strftime(timechar,sizeof timechar,"%d/%m/%Y, %H:%M:%S ",&tmbuf);

    if (z == 0) {
        fprintf(stderr,"Could not determine log time...\n");
    }

    else {
        log.append("%s",timechar);
    }

    log.append("%s - %s:%d ",getloglevel(lvl),file,line);
    log.append_v(fmt,args);

    if (errnum > 0) {
        //in accordance with GNU, strerror_r returns a const char *, POSIX based environments return an int
        //for errors not in the internal error string table, the temp buffer is filled with unknown err string

        char tempbuf[256];
        log.append(" (%d : %s)",errnum,strerror_r(errnum,tempbuf,sizeof tempbuf));
    
    }

    log.done();
}

void log_write(loglevel lvl,const char * file,int line,const char * fmt,...) {
    va_list args;
    va_start(args,fmt);
    vlog_write(lvl,-1,file,line,fmt,args);
    va_end(args);
}

void log_write(loglevel lvl,int errnum,const char * file,int line,const char * fmt,...) {
    va_list args;
    va_start(args,fmt);
    vlog_write(lvl,errnum,file,line,fmt,args);
    va_end(args);
}