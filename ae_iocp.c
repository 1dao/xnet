// ae_iocp.c 

#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <io.h>
#include "ae.h"
#include "zmalloc.h"

typedef struct aeApiState {
    HANDLE iocp;                    // IOCP句柄
    int eventCount;                 // 当前事件数量
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

static void aeApiFree(aeEventLoop* eventLoop) {
    aeApiState* state = eventLoop->apidata;

    if (state) {
        if (state->iocp) {
            CloseHandle(state->iocp);
        }
        zfree(state);
    }
}

static int aeApiAddEvent(aeEventLoop* eventLoop, int fd, int mask, aeFileEvent* fe) {
    aeApiState* state = eventLoop->apidata;
    SOCKET socket = (SOCKET)fd;
    if (CreateIoCompletionPort((HANDLE)socket, state->iocp, (ULONG_PTR)fe, 0) == NULL) {
        return -1;
    }
    state->eventCount++;
    return 0;
}

static void aeApiDelEvent(aeEventLoop* eventLoop, int fd, int mask) {
    aeApiState* state = eventLoop->apidata;
    state->eventCount--;
}

static int aeApiPoll(aeEventLoop* eventLoop, struct timeval* tvp) {
    aeApiState* state = eventLoop->apidata;
    DWORD timeout = tvp ? (tvp->tv_sec * 1000 + tvp->tv_usec / 1000) : INFINITE; 
    int numevents = 0;

    if (state->eventCount == 0) {
        Sleep(timeout);
        return 0;
    }

    DWORD bytesTransferred = 0;
    ULONG_PTR completionKey = 0;
    LPOVERLAPPED overlapped = NULL;

    // 获取完成状态
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

            printf("Queued event: mask=%d, trans=%d\n", mask, bytesTransferred);
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

static char* aeApiName(void) {
    return "iocp";
}
