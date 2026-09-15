#pragma once
#include "platform/Log.h"
#include <cstdarg>
#include <cstdio>

inline void MyLog(const char* fmt, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    SIGFLOW_LOG(buffer);
}
