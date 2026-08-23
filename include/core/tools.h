#pragma once
#include <netdb.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <stdint.h>
void fillIP(char * buf,size_t buflen,const sockaddr_in& addr);
void filltag(char * buf,size_t buflen,const sockaddr_in& addr,uint64_t pid);