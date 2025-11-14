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

#endif
