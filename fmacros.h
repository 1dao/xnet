#ifndef _REDIS_FMACRO_H
#define _REDIS_FMACRO_H

#ifdef _WIN32
#include <WinSock2.h>
#endif

#define _BSD_SOURCE

#if defined(__linux__) || defined(__OpenBSD__)
#define _XOPEN_SOURCE 700
#else
#define _XOPEN_SOURCE
#endif

#define _LARGEFILE_SOURCE
#define _FILE_OFFSET_BITS 64
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member))) 
#ifdef _WIN32
	typedef SOCKET				xSocket;
#else
	typedef int					xSocket;
#endif

#ifdef _WIN32
    #include <stdint.h>
    // 或者对于较老版本的 MSVC
#ifndef _MSC_VER
    #include <stdint.h>
    #else
        // MSVC 2008 及更早版本可能需要自定义定义
        typedef unsigned char uint8_t;
        typedef unsigned short uint16_t;
        typedef unsigned int uint32_t;
        typedef unsigned long long uint64_t;
    #endif
#else
    #include <stdint.h>
    #include <stddef.h>
    #ifndef _WIN32
        // Linux/Unix 平台类型定义
        #ifndef BOOL
            #define BOOL int
            #define TRUE 1
            #define FALSE 0
        #endif
    #endif
#endif
#endif
