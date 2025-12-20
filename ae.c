/* A simple event-driven programming library. Originally I wrote this code
 * for the Jim's event-loop (Jim is a Tcl interpreter) but later translated
 * it in form of a library for easy reuse.
 *
 * Copyright (c) 2006-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <stdio.h>
#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
    #include <sys/time.h>
    #include <unistd.h>
    #include <time.h>
    #include <search.h>
    #include <sys/socket.h>  // For AF_UNIX, SOCK_STREAM
    #include <fcntl.h>       // For fcntl, F_SETFL, O_NONBLOCK
#else
    #include <corecrt_search.h>
#endif
#include "ae.h"
#include "zmalloc.h"
#include "xtimer.h"

/* main aeEventLoop */
#ifdef _WIN32
static _declspec(thread) aeEventLoop* _net_ae = NULL;
#else
static __thread aeEventLoop* _net_ae = NULL;
#endif

// forward declaration
static int aeApiCreate(aeEventLoop* eventLoop);
static void aeApiFree(aeEventLoop* eventLoop);
static int aeApiResize(aeEventLoop* eventLoop, int setsize);
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp);
static int aeApiAddEvent(aeEventLoop* eventLoop, xSocket fd, int mask, aeFileEvent* fe);
static void aeApiDelEvent(aeEventLoop* eventLoop, xSocket fd, int mask);
static xSocket aeApiGetStateFD(aeEventLoop* eventLoop);
static char* aeApiName(void);

#define INITIAL_EVENT 1024
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;
    if (_net_ae) return _net_ae;
    eventLoop = (aeEventLoop*)zmalloc(sizeof(*eventLoop));
    if (!eventLoop) return NULL;
    memset(eventLoop, 0x00, sizeof(*eventLoop));
    eventLoop->nevents = setsize < INITIAL_EVENT ? setsize : INITIAL_EVENT;
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*eventLoop->nevents);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*eventLoop->nevents);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto ERR_RET;
    eventLoop->setsize = setsize;
    eventLoop->stop = 0;
    eventLoop->maxfd = 0;
    eventLoop->beforesleep = NULL;
    eventLoop->efhead = 0;
    eventLoop->fdWaitSlot = -1;
    if (aeApiCreate(eventLoop) == -1) goto ERR_RET;
    /* Events with mask == AE_NONE are not set. So let's initialize the
     * vector with it. */
    for (i = 0; i < eventLoop->nevents; i++) {
        eventLoop->events[i].slot = i + 1;
        eventLoop->events[i].mask = AE_NONE;
    }
    eventLoop->events[eventLoop->nevents-1].slot = -1;     // last event

    _net_ae = eventLoop;
    return eventLoop;
ERR_RET:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* Return the current set size. */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/* Resize the maximum set size of the event loop.
 * If the requested set size is smaller than the current set size, but
 * there is already a file descriptor in use that is >= the requested
 * set size minus one, AE_ERR is returned and the operation is not
 * performed at all.
 *
 * Otherwise AE_OK is returned and the operation is successful. */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    eventLoop->setsize = setsize;

    /* If the current allocated space is larger than the requested size,
     * we need to shrink it to the requested size. */
    if (setsize < eventLoop->nevents) {
        eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
        eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
        eventLoop->nevents = setsize;
    }
    return AE_OK;
}

/*
 * Return the current event loop.
 *
 * Note: it just means you turn on/off the global AE_DONT_WAIT.
 */
aeEventLoop* aeGetCurEventLoop(void) {
    if (!_net_ae)
        aeCreateEventLoop(INITIAL_EVENT);
    return _net_ae;
}

void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    if (!eventLoop) return;
#ifndef HAVE_IOCP
    if (eventLoop->signal_fd[0] >= 0) {
        close(eventLoop->signal_fd[0]);
        close(eventLoop->signal_fd[1]);
    }
#endif
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);
    zfree(eventLoop);

    if (_net_ae == eventLoop)
        _net_ae = NULL;
}


void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}

int aeCreateFileEvent(aeEventLoop *eventLoop, xSocket fd, int mask,
    aeFileProc *proc, void *clientData, aeFileEvent** ev) {
    //if (fd >= AE_SETSIZE) return AE_ERR;
    if (eventLoop->efhead == -1) return AE_ERR;
    /* Resize the events and fired arrays if the file
     * descriptor exceeds the current number of events. */
    if (unlikely(fd >= eventLoop->nevents)) {
        int newnevents = eventLoop->nevents;
        newnevents = (newnevents * 2 > (int)fd + 1) ? newnevents * 2 : (int)fd + 1;
        newnevents = (newnevents > eventLoop->setsize) ? eventLoop->setsize : newnevents;
        eventLoop->events = zrealloc(eventLoop->events, sizeof(aeFileEvent) * newnevents);
        eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * newnevents);

        /* Initialize new slots with an AE_NONE mask */
        for (int i = eventLoop->nevents; i < newnevents; i++)
            eventLoop->events[i].mask = AE_NONE;
        eventLoop->nevents = newnevents;
    }
    aeFileEvent *fe = &eventLoop->events[eventLoop->efhead];
    eventLoop->efhead = fe->slot;
    if(ev)
        *ev = fe;

    if (aeApiAddEvent(eventLoop, fd, mask, fe) == -1){
        return AE_ERR;
    }

    fe->mask |= mask;
    if (mask & AE_READABLE) fe->rfileProc = proc;
    if (mask & AE_WRITABLE) fe->wfileProc = proc;
    fe->clientData = clientData?clientData:fd;
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

void aeDeleteFileEvent(aeEventLoop* eventLoop, xSocket fd, aeFileEvent* fe, int mask) {
    if (fe->mask == AE_NONE) return;

    fe->mask = fe->mask & (~mask);
    if (fe->mask == AE_NONE) {
        if (fd == eventLoop->maxfd) {
            /* Update the max fd */
            int j = 0;
            for (j = (int)eventLoop->maxfd - 1; j >= 0; j--)
                if (eventLoop->events[j].mask != AE_NONE) break;
            eventLoop->maxfd = j;
        }
        aeApiDelEvent(eventLoop, fd, mask);

        // swap to free node
        if (fe->slot != eventLoop->efhead) {
            short int slot = fe->slot;
            fe->slot = eventLoop->efhead;
            eventLoop->efhead = slot;
        }
    }
}

/* Process every pending time event, then every pending file event
 * (that may be registered by time event callbacks just processed).
 * Without special flags the function sleeps until some file event
 * fires, or when the next time event occurrs (if any).
 *
 * If flags is 0, the function does nothing and returns.
 * if flags has AE_ALL_EVENTS set, all the kind of events are processed.
 * if flags has AE_FILE_EVENTS set, file events are processed.
 * if flags has AE_TIME_EVENTS set, time events are processed.
 * if flags has AE_DONT_WAIT set the function returns ASAP until all
 * the events that's possible to process without to wait are processed.
 *
 * The function returns the number of events processed. */
int aeProcessEvents(aeEventLoop *eventLoop, int flags) {
    int processed = 0, numevents;

    /* Nothing to do? return ASAP */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* Note that we want call select() even if there are no
     * file events to process as long as we want to process time
     * events, in order to sleep until the next time event is ready
     * to fire. */
    if (eventLoop->maxfd != 0 || eventLoop->fdWaitSlot !=-1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        struct timeval tv, *tvp;

        int interval = -1;
        if (flags & AE_TIME_EVENTS)
            interval = xtimer_last();
        if (interval >= 0) {
            tvp = &tv;
            if (flags & AE_DONT_WAIT) {
                tvp->tv_sec = 0;
                tvp->tv_usec = interval > 10 ?(10000):(interval*1000);
            } else {
                tvp->tv_sec = interval / 1000;
                tvp->tv_usec = (interval % 1000) * 1000;
            }
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to se the timeout
             * to zero, force 100fps*/
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = 0;
                tv.tv_usec = 10000;
                tvp = &tv;
            } else {
                /* Otherwise we can block */
                tvp = NULL; /* wait forever */
            }
        }

        numevents = aeApiPoll(eventLoop, tvp);
        for (j = 0; j < numevents; j++) {
            aeFileEvent* fe = eventLoop->fired[j].fe;// &eventLoop->events[eventLoop->fired[j].fd];
            int mask = eventLoop->fired[j].mask;
            xSocket fd = eventLoop->fired[j].fd;
            int trans = eventLoop->fired[j].trans;
            int rfired = 0;

	        /* note the fe->mask & mask & ... code: maybe an already processed
             * event removed an element that fired and we still didn't
             * processed, so we check if the event is still valid. */
            if (fe->mask & mask & AE_READABLE) {
                rfired = 1;
                fe->rfileProc(eventLoop, fd, fe->clientData, mask, trans);
            }
            if (fe->mask & mask & AE_WRITABLE) {
                if (!rfired || fe->wfileProc != fe->rfileProc)
                    fe->wfileProc(eventLoop, fd, fe->clientData, mask, trans);
            }
            processed++;
        }
    }

    /* Check time events */
    if (flags & AE_TIME_EVENTS)
        xtimer_update();

    return processed; /* return the number of processed file/time events */
}

/* Wait for millseconds until the given file descriptor becomes
 * writable/readable/exception */
int aeWait(xSocket fd, int mask, long long milliseconds) {
    struct timeval tv;
    fd_set rfds, wfds, efds;
    int retmask = 0, retval;

    tv.tv_sec = (long)milliseconds / 1000;
    tv.tv_usec = (long)(milliseconds % 1000) * 1000;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    if (mask & AE_READABLE) FD_SET(fd, &rfds);
    if (mask & AE_WRITABLE) FD_SET(fd, &wfds);
    if ((retval = select((int)fd + 1, &rfds, &wfds, &efds, &tv)) > 0) {
        if (FD_ISSET(fd, &rfds)) retmask |= AE_READABLE;
        if (FD_ISSET(fd, &wfds)) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        if (eventLoop->beforesleep != NULL)
            eventLoop->beforesleep(eventLoop);
        aeProcessEvents(eventLoop, AE_ALL_EVENTS);
    }
}

char *aeGetApiName(void) {
    return aeApiName();
}

void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

#ifndef HAVE_IOCP
static int aeSignalProc(struct aeEventLoop *eventLoop, xSocket fd, void *clientData, int mask, int trans) {
    char buf[64];
    while (read((int)clientData, buf, sizeof(buf)) > 0);
    return AE_OK;
}
#endif

void aeCreateSignalFile(aeEventLoop* eventLoop) {
    eventLoop->fdWaitSlot = eventLoop->efhead;
#ifndef HAVE_IOCP
    if (eventLoop->signal_fd[0] != 0) return;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, eventLoop->signal_fd) == 0) {
        fcntl(eventLoop->signal_fd[0], F_SETFL, O_NONBLOCK);
        fcntl(eventLoop->signal_fd[1], F_SETFL, O_NONBLOCK);

        aeCreateFileEvent(eventLoop, eventLoop->signal_fd[1], AE_READABLE, aeSignalProc, NULL, NULL);
    }
#else
    aeApiAddEvent(eventLoop, -1, 0, NULL);
#endif
}

void aeDeleteSignalFile(aeEventLoop* eventLoop) {
    int slot = eventLoop->fdWaitSlot;
    if (slot == -1) return;
    eventLoop->fdWaitSlot = -1;

#ifndef HAVE_IOCP
    aeDeleteFileEvent(eventLoop, eventLoop->signal_fd[1], &eventLoop->events[slot], AE_READABLE);
#else
    aeApiDelEvent(eventLoop, -1, 0);
#endif
}

void aeGetSignalFile(aeEventLoop *eventLoop, xSocket* fdSignal){
#ifndef HAVE_IOCP
    *fdSignal = eventLoop->signal_fd[0];
#else
    *fdSignal = aeApiGetStateFD(eventLoop);
#endif
}


// all ae implementation
//
#ifdef HAVE_IOCP
// ae_iocp.c
#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <io.h>
#include <stdio.h>
#include "ae.h"
#include "zmalloc.h"

typedef struct aeApiState {
    HANDLE iocp;
    int eventCount;
} aeApiState;

static int aeApiCreate(aeEventLoop* eventLoop) {
    aeApiState* state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;

    state->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (state->iocp == NULL) {
        zfree(state);
        return -1;
    }

    state->eventCount = 0;
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop* eventLoop, int setsize) {
    (void)(eventLoop);
    (void)(setsize);
    return 0;
}

static void aeApiFree(aeEventLoop* eventLoop) {
    aeApiState* state = eventLoop->apidata;

    if (state) {
        if (state->iocp) {
            CloseHandle(state->iocp);
        }
        zfree(state);
    }
}

static int aeApiAddEvent(aeEventLoop* eventLoop, xSocket fd, int mask, aeFileEvent* fe) {
    aeApiState* state = eventLoop->apidata; // -1 for signal notify
    if ((int)fd != -1 && CreateIoCompletionPort((HANDLE)fd, state->iocp, (ULONG_PTR)fe, 0) == NULL) {
        return -1;
    }
    state->eventCount++;
    return 0;
}

static void aeApiDelEvent(aeEventLoop* eventLoop, xSocket fd, int mask) {
    aeApiState* state = eventLoop->apidata;
    state->eventCount--;
}

static int aeApiPoll(aeEventLoop* eventLoop, struct timeval* tvp) {
    aeApiState* state = eventLoop->apidata;
    DWORD timeout = tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : INFINITE;
    int numevents = 0;

    if (state->eventCount == 0 && state) {
        Sleep(timeout);
        return 0;
    }

    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    LPOVERLAPPED overlapped = NULL;

    BOOL result = GetQueuedCompletionStatus(
        state->iocp,
        &bytesTransferred,
        &completionKey,
        &overlapped,
        timeout
    );

    if (result && overlapped != NULL) {
        int mask = *((int*)((char*)overlapped + sizeof(OVERLAPPED)));
        if (mask != 0) {
            eventLoop->fired[numevents].fd = -1;
            eventLoop->fired[numevents].mask = mask;
            eventLoop->fired[numevents].fe = (aeFileEvent*)completionKey;
            eventLoop->fired[numevents].trans = bytesTransferred;
            numevents++;

            printf("Queued event: mask=%d, trans=%d\n", mask, (int)bytesTransferred);
        }
    }
    else if (!result && overlapped != NULL) {
        int error = GetLastError();
        printf("GetQueuedCompletionStatus failed: error=%d\n", error);

        int mask = *((int*)((char*)overlapped + sizeof(OVERLAPPED)));
        if (mask != 0) {
            eventLoop->fired[numevents].fd = -1;
            eventLoop->fired[numevents].mask = mask;
            eventLoop->fired[numevents].fe = (aeFileEvent*)completionKey;
            eventLoop->fired[numevents].trans = 0; // 传输失败
            numevents++;
        }
    }

    return numevents;
}

static xSocket aeApiGetStateFD(aeEventLoop* eventLoop) {
    aeApiState* state = (aeApiState*)eventLoop->apidata;
    return (xSocket)state->iocp;
}

static char* aeApiName(void) {
    return "iocp";
}
#elif defined(HAVE_KQUEUE)
/* Kqueue(2)-based ae.c module
 *
 * Copyright (C) 2009 Harish Mallipeddi - harish.mallipeddi@gmail.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
__attribute__((noreturn))
void panic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(EXIT_FAILURE);
}

typedef struct aeApiState {
    int kqfd;
    struct kevent* events;

    /* Events mask for merge read and write event.
     * To reduce memory consumption, we use 2 bits to store the mask
     * of an event, so that 1 byte will store the mask of 4 events. */
    char* eventsMask;
} aeApiState;

#define EVENT_MASK_MALLOC_SIZE(sz) (((sz) + 3) / 4)
#define EVENT_MASK_OFFSET(fd) ((fd) % 4 * 2)
#define EVENT_MASK_ENCODE(fd, mask) (((mask) & 0x3) << EVENT_MASK_OFFSET(fd))

static inline int getEventMask(const char* eventsMask, int fd) {
    return (eventsMask[fd / 4] >> EVENT_MASK_OFFSET(fd)) & 0x3;
}

static inline void addEventMask(char* eventsMask, int fd, int mask) {
    eventsMask[fd / 4] |= EVENT_MASK_ENCODE(fd, mask);
}

static inline void resetEventMask(char* eventsMask, int fd) {
    eventsMask[fd / 4] &= ~EVENT_MASK_ENCODE(fd, 0x3);
}

static int aeApiCreate(aeEventLoop* eventLoop) {
    aeApiState* state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct kevent) * eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->kqfd = kqueue();
    if (state->kqfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    //anetCloexec(state->kqfd);
    state->eventsMask = zmalloc(EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(eventLoop->setsize));
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop* eventLoop, int setsize) {
    aeApiState* state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct kevent) * setsize);
    state->eventsMask = zrealloc(state->eventsMask, EVENT_MASK_MALLOC_SIZE(setsize));
    memset(state->eventsMask, 0, EVENT_MASK_MALLOC_SIZE(setsize));
    return 0;
}

static void aeApiFree(aeEventLoop* eventLoop) {
    aeApiState* state = eventLoop->apidata;

    close(state->kqfd);
    zfree(state->events);
    zfree(state->eventsMask);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop* eventLoop, int fd, int mask, aeFileEvent* fe) {
    aeApiState* state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_ADD, 0, 0, (void*)fe);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_ADD, 0, 0, (void*)fe);
        if (kevent(state->kqfd, &ke, 1, NULL, 0, NULL) == -1) return -1;
    }
    return 0;
}

static void aeApiDelEvent(aeEventLoop* eventLoop, int fd, int mask) {
    aeApiState* state = eventLoop->apidata;
    struct kevent ke;

    if (mask & AE_READABLE) {
        EV_SET(&ke, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
    if (mask & AE_WRITABLE) {
        EV_SET(&ke, fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);
        kevent(state->kqfd, &ke, 1, NULL, 0, NULL);
    }
}

static int aeApiPoll(aeEventLoop* eventLoop, struct timeval* tvp) {
    aeApiState* state = eventLoop->apidata;
    int retval, numevents = 0;

    if (tvp != NULL) {
        struct timespec timeout;
        timeout.tv_sec = tvp->tv_sec;
        timeout.tv_nsec = tvp->tv_usec * 1000;
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
            &timeout);
    }
    else {
        retval = kevent(state->kqfd, NULL, 0, state->events, eventLoop->setsize,
            NULL);
    }

    if (retval > 0) {
        int j;

        /* Normally we execute the read event first and then the write event.
         * When the barrier is set, we will do it reverse.
         *
         * However, under kqueue, read and write events would be separate
         * events, which would make it impossible to control the order of
         * reads and writes. So we store the event's mask we've got and merge
         * the same fd events later. */
        for (j = 0; j < retval; j++) {
            struct kevent* e = state->events + j;
            int fd = e->ident;
            int mask = 0;

            if (e->filter == EVFILT_READ) mask = AE_READABLE;
            else if (e->filter == EVFILT_WRITE) mask = AE_WRITABLE;
            addEventMask(state->eventsMask, fd, mask);
        }

        /* Re-traversal to merge read and write events, and set the fd's mask to
         * 0 so that events are not added again when the fd is encountered again. */
        numevents = 0;
        for (j = 0; j < retval; j++) {
            struct kevent* e = state->events + j;
            int fd = e->ident;
            int mask = getEventMask(state->eventsMask, fd);
            if (mask) {
                eventLoop->fired[numevents].fd = fd;
                eventLoop->fired[numevents].mask = mask;
                eventLoop->fired[numevents].trans = (int)e->data;
                eventLoop->fired[numevents].fe = (aeFileEvent*)e->udata;
                resetEventMask(state->eventsMask, fd);
                numevents++;
            }
        }
    }
    else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: kevent, %s", strerror(errno));
    }

    return numevents;
}

static char* aeApiName(void) {
    return "kqueue";
}
#elif defined(HAVE_EPOLL)
/* Linux epoll(2) based ae.c module
* Copyright (C) 2009-2010 Salvatore Sanfilippo - antirez@gmail.com
* Released under the BSD license. See the COPYING file for more info. */

#include <sys/epoll.h>
#include <errno.h>


#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
__attribute__((noreturn))
void panic(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "PANIC: ");
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
    exit(EXIT_FAILURE);
}

typedef struct aeApiState {
    int epfd;
    struct epoll_event* events;
} aeApiState;

static int aeApiCreate(aeEventLoop* eventLoop) {
    aeApiState* state = zmalloc(sizeof(aeApiState));
    if (!state) return -1;
    state->events = zmalloc(sizeof(struct epoll_event) * eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }

    state->epfd = epoll_create(1024); /* 1024 is just an hint for the kernel */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    eventLoop->apidata = state;
    return 0;
}

static int aeApiResize(aeEventLoop* eventLoop, int setsize) {
    aeApiState* state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct epoll_event) * setsize);
    return 0;
}

static void aeApiFree(aeEventLoop* eventLoop) {
    aeApiState* state = (aeApiState*)eventLoop->apidata;
    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

static int aeApiAddEvent(aeEventLoop* eventLoop, int fd, int mask, aeFileEvent* fe) {
    aeApiState* state = (aeApiState*)eventLoop->apidata;
    struct epoll_event ee = { 0 };
    /* If the fd was already monitored for some event, we need a MOD
     * operation. Otherwise we need an ADD operation. */
    int op = fe->mask == AE_NONE ?
        EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= fe->mask; /* Merge old events */
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.ptr = fe;
    if (epoll_ctl(state->epfd, op, fd, &ee) == -1) return -1;
    return 0;
}

static void aeApiDelEvent(aeEventLoop* eventLoop, int fd, int delmask) {
    aeApiState* state = (aeApiState*)eventLoop->apidata;
    struct epoll_event ee;
    int mask = eventLoop->events[fd].mask & (~delmask);

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.u64 = 0; /* avoid valgrind warning */
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd, EPOLL_CTL_MOD, fd, &ee);
    }
    else {
        /* Note, Kernel < 2.6.9 requires a non null event pointer even for
         * EPOLL_CTL_DEL. */
        epoll_ctl(state->epfd, EPOLL_CTL_DEL, fd, &ee);
    }
}

static int aeApiPoll(aeEventLoop* eventLoop, struct timeval* tvp) {
    aeApiState* state = (aeApiState*)eventLoop->apidata;
    int retval, numevents = 0;

    retval = epoll_wait(state->epfd, state->events, eventLoop->setsize,
        tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event* e = state->events + j;

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            eventLoop->fired[j].fd = e->data.fd;
            eventLoop->fired[j].mask = mask;
            eventLoop->fired[j].fe = (aeFileEvent*)e->data.ptr;
        }
    }
    else if (retval == -1 && errno != EINTR) {
        panic("aeApiPoll: epoll_wait, %s", strerror(errno));
    }
    return numevents;
}

static xSocket aeApiGetStateFD(aeEventLoop* eventLoop) {
    aeApiState* state = (aeApiState*)eventLoop->apidata;
    return (xSocket)state->epfd;
}

static char* aeApiName(void) {
    return (char*)"epoll";
}
#else
    #error "Your operating system does not support any of the event loops available."
#endif
