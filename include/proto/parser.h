#pragma once
#include <unistd.h>
#include <stdint.h>
#include "core/log.h"

int handleauth();
int parse(uint8_t* readbuffer,size_t& readpointer,size_t& writepointer,const char * tag = nullptr);