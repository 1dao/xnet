#ifndef _XTIMER_H_
#define _XTIMER_H_

#ifdef __cplusplus
extern "C" {
#endif

#include "xheapmin.h"

typedef void (*fnOnTime)(void*);
typedef void* xtimerHandler;

// api
void xtimer_init(int cap);
void xtimer_uninit();
int  xtimer_update();
void xtimer_show();

xtimerHandler xtimer_add(int interval_ms, const char* name, fnOnTime callback, void* ud, int repeat_num);
void          xtimer_del(xtimerHandler handler);

// utils
#ifdef _WIN32
#include <windows.h>
static long64 time_get_ms() {
    return (long64)GetTickCount64();
}

static long64 time_get_us() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    return (uli.QuadPart / 10);
}
#else
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static long64 time_get_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (long64)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long64 time_get_us() {
    struct timeval tv;
    gettimeofday(&tv, NULL);

    return (long64)tv.tv_sec * 1000000LL + tv.tv_usec;
}
#endif

#ifdef __cplusplus
}
#endif
#endif // _XTIMER_H_
