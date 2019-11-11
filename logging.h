/*
 * 定义日志输出
 */

#ifndef _LOGGING_H_
#define _LOGGING_H_

#include<stdio.h>
#include<iostream>

//log levels
#define DEBUG 4
#define TRACE 8
#define INFO 12
#define WARNING 16
#define ERROR 20
#define FATAL 24
#define OFF 28

//log level in use
#define LOG_LEVEL DEBUG

#define LOG_LEVEL_CHECK(level) ((level < LOG_LEVEL) ? 0 : 1)
#define LOG(level) LOG_LEVEL_CHECK(level) && std::cout
    //<< "[" << __FILE__ << ":" << __LINE__ << ":" \
    //<< __func__ << "]"
#define LOGP(level,format,...) LOG_LEVEL_CHECK(level) && \
    printf(format,##__VA_ARGS__)

#endif