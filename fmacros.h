#ifndef _REDIS_FMACRO_H
#define _REDIS_FMACRO_H

#ifdef _WIN32
#ifndef _WINSOCK_DEPRECATED_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#endif
#include <WinSock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#endif

#define _BSD_SOURCE

#if defined(__linux__) || defined(__OpenBSD__)
#define _XOPEN_SOURCE 700
#else
#define _XOPEN_SOURCE
#endif

#ifndef _LARGEFILE_SOURCE
#define _LARGEFILE_SOURCE
#endif
#define _FILE_OFFSET_BITS 64
#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))
#ifdef _WIN32
	typedef SOCKET				xSocket;
#else
	typedef int					xSocket;
#endif
#define UNUSED(x) (void)(x)

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

    // Linux/Unix 平台类型定义
    #ifndef BOOL
        #define BOOL int
        #define TRUE 1
        #define FALSE 0
    #endif
#endif

// unlikely/likely define
#ifndef unlikely
    #if defined(__GNUC__) || defined(__clang__)
        // GCC/Clang：使用 __builtin_expect
        #define unlikely(x) __builtin_expect(!!(x), 0)
        #define likely(x)   __builtin_expect(!!(x), 1)
    #elif defined(__FreeBSD__) || defined(__APPLE__)
        // FreeBSD/macOS：使用 sys/cdefs.h 封装
        #include <sys/cdefs.h>
    //#elif defined(_MSC_VER) && _MSC_VER >= 1910  // VS2017+
    //    // MSVC 2017+：使用 _unlikely/_likely
    //    #include <intrin.h>
    //    #define unlikely(x) _unlikely(x)
    //    #define likely(x)   _likely(x)
    #else
        // 其他编译器（IAR、低版本MSVC等）：空定义
        #define unlikely(x) (x)
        #define likely(x)   (x)
    #endif
#endif

#ifdef __linux__
#define HAVE_EPOLL 1
#endif

/* Macros */
#define AE_NOTUSED(V) ((void) V)
#ifdef _WIN32
    #define AE_USING_IOCP
#endif

#endif
