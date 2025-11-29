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

/* Include the best multiplexing layer supported by this system.
 * The following should be ordered by performances, descending. */
#ifdef HAVE_EPOLL
	#include "ae_epoll.c"
#else
	#ifdef HAVE_KQUEUE
	    #include "ae_kqueue.c"
	#else
	    #ifdef _WIN32
            #ifdef AE_USING_IOCP
                #include "ae_iocp.c"
            #else
                #include "ae_ws2.c"
            #endif
	    #else
		    #include "ae_select.c"
	    #endif
	#endif
#endif

/* main aeEventLoop */
#ifdef _WIN32
static _declspec(thread) aeEventLoop* _net_ae = NULL;
#else
static __thread aeEventLoop* _net_ae = NULL;
#endif

#ifndef AE_USING_IOCP
static int aeSignalProc(struct aeEventLoop *eventLoop, xSocket fd, void *clientData, int mask, int trans) {
    char buf[64];
    // 读取信号数据，避免fd一直处于可读状态
    while (read((int)clientData, buf, sizeof(buf)) > 0);
    return AE_OK;
}
#endif

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
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = 0;
    eventLoop->beforesleep = NULL;
    eventLoop->efhead = 0;
    eventLoop->nrpc = 0;
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
#ifndef AE_USING_IOCP
    if (eventLoop->signal_fd[0] >= 0) {
        close(eventLoop->signal_fd[0]);
        close(eventLoop->signal_fd[1]);
    }
#endif
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);

    /* Free the time events list. */
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        if (te->finalizerProc)
            te->finalizerProc(eventLoop, te->clientData);
        zfree(te);
        te = next_te;
    }
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

static void aeGetTime(long *seconds, long *milliseconds) {
#ifdef _WIN32
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER uli;
    uli.LowPart = ft.dwLowDateTime;
    uli.HighPart = ft.dwHighDateTime;

    const unsigned long long EPOCH_DIFF = 11644473600ULL;
    const unsigned long long HUNDRED_NS_PER_SEC = 10000000ULL;
    const unsigned long long HUNDRED_NS_PER_USEC = 10ULL;
    unsigned long long total_hundred_ns = uli.QuadPart - EPOCH_DIFF * HUNDRED_NS_PER_SEC;

    *seconds = (long)(total_hundred_ns / HUNDRED_NS_PER_SEC);
    *milliseconds = (long)((total_hundred_ns % HUNDRED_NS_PER_SEC) / HUNDRED_NS_PER_USEC);
#else
    struct timeval tv;

    gettimeofday(&tv, NULL);
    *seconds = tv.tv_sec;
    *milliseconds = tv.tv_usec / 1000;
#endif
}

static void aeAddMillisecondsToNow(long long milliseconds, long *sec, long *ms) {
    long cur_sec, cur_ms, when_sec, when_ms;

    aeGetTime(&cur_sec, &cur_ms);
    when_sec = cur_sec + (long)(milliseconds/1000);
    when_ms = cur_ms + (long)(milliseconds%1000);
    if (when_ms >= 1000) {
        when_sec ++;
        when_ms -= 1000;
    }
    *sec = when_sec;
    *ms = when_ms;
}

long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc) {
    long long id = eventLoop->timeEventNextId++;
    aeTimeEvent *te;

    te = (aeTimeEvent *)zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    aeAddMillisecondsToNow(milliseconds,&te->when_sec,&te->when_ms);
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    te->next = eventLoop->timeEventHead;
    eventLoop->timeEventHead = te;
    return id;
}

int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id) {
    aeTimeEvent *te, *prev = NULL;

    te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            if (prev == NULL)
                eventLoop->timeEventHead = te->next;
            else
                prev->next = te->next;
            if (te->finalizerProc)
                te->finalizerProc(eventLoop, te->clientData);
            zfree(te);
            return AE_OK;
        }
        prev = te;
        te = te->next;
    }
    return AE_ERR; /* NO event with the specified ID found */
}

/* Search the first timer to fire.
 * This operation is useful to know how many time the select can be
 * put in sleep without to delay any event.
 * If there are no timers NULL is returned.
 *
 * Note that's O(N) since time events are unsorted.
 * Possible optimizations (not needed by Redis so far, but...):
 * 1) Insert the event in order, so that the nearest is just the head.
 *    Much better but still insertion or deletion of timers is O(N).
 * 2) Use a skiplist to have this operation as O(1) and insertion as O(log(N)).
 */
static aeTimeEvent *aeSearchNearestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    aeTimeEvent *nearest = NULL;

    while(te) {
        if (!nearest || te->when_sec < nearest->when_sec ||
                (te->when_sec == nearest->when_sec &&
                 te->when_ms < nearest->when_ms))
            nearest = te;
        te = te->next;
    }
    return nearest;
}

/* Process time events */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;
    while(te) {
        long now_sec, now_ms;
        long long id;

        if (te->id > maxId) {
            te = te->next;
            continue;
        }
        aeGetTime(&now_sec, &now_ms);
        if (now_sec > te->when_sec ||
            (now_sec == te->when_sec && now_ms >= te->when_ms))
        {
            int retval;

            id = te->id;
            retval = te->timeProc(eventLoop, id, te->clientData);
            processed++;
            /* After an event is processed our time event list may
             * no longer be the same, so we restart from head.
             * Still we make sure to don't process events registered
             * by event handlers itself in order to don't loop forever.
             * To do so we saved the max ID we want to handle.
             *
             * FUTURE OPTIMIZATIONS:
             * Note that this is NOT great algorithmically. Redis uses
             * a single time event so it's not a problem but the right
             * way to do this is to add the new elements on head, and
             * to flag deleted elements in a special way for later
             * deletion (putting references to the nodes to delete into
             * another linked list). */
            if (retval != AE_NOMORE) {
                aeAddMillisecondsToNow(retval,&te->when_sec,&te->when_ms);
            } else {
                aeDeleteTimeEvent(eventLoop, id);
            }
            te = eventLoop->timeEventHead;
        } else {
            te = te->next;
        }
    }
    return processed;
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
        aeTimeEvent *shortest = NULL;
        struct timeval tv, *tvp;

        if (flags & AE_TIME_EVENTS && !(flags & AE_DONT_WAIT))
            shortest = aeSearchNearestTimer(eventLoop);
        if (shortest) {
            long now_sec, now_ms;

            /* Calculate the time missing for the nearest
             * timer to fire. */
            aeGetTime(&now_sec, &now_ms);
            tvp = &tv;
            tvp->tv_sec = shortest->when_sec - now_sec;
            if (shortest->when_ms < now_ms) {
                tvp->tv_usec = ((shortest->when_ms+1000) - now_ms)*1000;
                tvp->tv_sec --;
            } else {
                tvp->tv_usec = (shortest->when_ms - now_ms)*1000;
            }
            if (tvp->tv_sec < 0) tvp->tv_sec = 0;
            if (tvp->tv_usec < 0) tvp->tv_usec = 0;
        } else {
            /* If we have to check for events but need to return
             * ASAP because of AE_DONT_WAIT we need to se the timeout
             * to zero */
            if (flags & AE_DONT_WAIT) {
                tv.tv_sec = tv.tv_usec = 0;
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
        processed += processTimeEvents(eventLoop);

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

    }
    else {
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


void aeCreateSignalFile(aeEventLoop* eventLoop) {
    eventLoop->fdWaitSlot = eventLoop->efhead;

#ifndef AE_USING_IOCP
    if (eventLoop->signal_fd[0] != 0) return;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, eventLoop->signal_fd) == 0) {
        // 设置非阻塞
        fcntl(eventLoop->signal_fd[0], F_SETFL, O_NONBLOCK);
        fcntl(eventLoop->signal_fd[1], F_SETFL, O_NONBLOCK);
        // 注册读事件
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

#ifndef AE_USING_IOCP
    aeDeleteFileEvent(eventLoop, eventLoop->signal_fd[1], &eventLoop->events[slot], AE_READABLE);
#else
    aeApiDelEvent(eventLoop, -1, 0);
#endif
}

void aeGetSignalFile(aeEventLoop *eventLoop, xSocket* fdSignal){
#ifndef AE_USING_IOCP
    *fdSignal = eventLoop->signal_fd[0];
#else
    *fdSignal = aeGetStateFD(eventLoop);
#endif
}
