/* IOCP-based ae.c module for Windows
 * 重新实现版本：使用iocp_context_t作为完成键
 */

#include <string.h>
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <io.h>
#include "ae.h"
#include "zmalloc.h"

 // AcceptEx 函数指针
static LPFN_ACCEPTEX lpAcceptEx = NULL;

typedef struct aeApiState {
    HANDLE iocp;                    // IOCP句柄
    int eventCount;                 // 当前事件数量
} aeApiState;

// 初始化AcceptEx
static int initializeAcceptEx(SOCKET listenSocket) {
    if (lpAcceptEx != NULL) return 0;

    GUID guidAcceptEx = WSAID_ACCEPTEX;
    DWORD bytesReturned = 0;

    if (WSAIoctl(listenSocket, SIO_GET_EXTENSION_FUNCTION_POINTER,
        &guidAcceptEx, sizeof(guidAcceptEx),
        &lpAcceptEx, sizeof(lpAcceptEx),
        &bytesReturned, NULL, NULL) == SOCKET_ERROR) {
        return -1;
    }

    return 0;
}

static int aeApiCreate(aeEventLoop* eventLoop) {
    aeApiState* state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;

    // 创建IOCP句柄
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

static int aeApiAddEvent(aeEventLoop* eventLoop, int fd, int mask) {
    // 这个函数保留给兼容性，实际使用aeApiAddEventEx
    return AE_ERR;
}

inline static int aePostIocpAccept(int socket, OVERLAPPED* overlapped) {
    // 创建接受socket
    SOCKET acceptSocket = WSASocket(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
    if (acceptSocket == INVALID_SOCKET) {
        return -1;
    }

    // 投递 AcceptEx
    DWORD bytesReceived = 0;
    if (lpAcceptEx(socket, acceptSocket,
        (char*)overlapped + sizeof(OVERLAPPED) + sizeof(int), // 使用缓冲区域
        0, sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
        &bytesReceived, overlapped) == FALSE) {
        int error = WSAGetLastError();
        if (error != ERROR_IO_PENDING) {
            closesocket(acceptSocket);
            return -1;
        }
    }

    // 在接受上下文中存储接受socket
    *((SOCKET*)((char*)overlapped + sizeof(OVERLAPPED))) = acceptSocket;
}

static int aeApiAddEventEx(aeEventLoop* eventLoop, int fd, int mask, void* clientData) {
    aeApiState* state = eventLoop->apidata;
    SOCKET socket = (SOCKET)fd;

    // 将socket与IOCP关联，完成键为clientData（iocp_context_t指针）
    if (CreateIoCompletionPort((HANDLE)socket, state->iocp, (ULONG_PTR)clientData, 0) == NULL) {
        return -1;
    }

    // 特殊处理监听socket - 投递AcceptEx
    if (mask & AE_ACCEPT) {
        if (initializeAcceptEx(socket) == -1) {
            return -1;
        }

        aePostAccept(fd, clientData);
    }
    // 为可读事件投递WSARecv操作
    else if (mask & AE_READABLE) {
        // 使用传入的clientData作为OVERLAPPED
        OVERLAPPED* overlapped = (OVERLAPPED*)clientData;
        WSABUF wsaBuf;
        DWORD bytesReceived = 0;
        DWORD flags = 0;

        // 设置缓冲区指针（在iocp_context_t中）
        char* buffer = (char*)overlapped + sizeof(OVERLAPPED) + sizeof(int);
        wsaBuf.buf = buffer;
        wsaBuf.len = 64 * 1024; // 与xnet_svr.c中的缓冲区大小一致

        ZeroMemory(overlapped, sizeof(OVERLAPPED));

        if (WSARecv(socket, &wsaBuf, 1, &bytesReceived, &flags, overlapped, NULL) == SOCKET_ERROR) {
            int error = WSAGetLastError();
            if (error != WSA_IO_PENDING) {
                return -1;
            }
        }
    }

    state->eventCount++;
    return 0;
}

static void aeApiDelEvent(aeEventLoop* eventLoop, int fd, int mask) {
    aeApiState* state = eventLoop->apidata;
    // IOCP中删除事件主要是取消未完成的IO操作
    // 实际清理在socket关闭时自动进行
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

    //while (numevents < AE_SETSIZE) {
    if (1) {
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

        if (!result) {
            //// 超时或错误
            //DWORD error = GetLastError();
            //if (error == WAIT_TIMEOUT) {
            //    break; // 正常超时
            //}
            //// 其他错误，继续处理下一个
            //continue;
        }

        //if (completionKey == 0 && overlapped == NULL) {
        //    // 特殊唤醒事件
        //    continue;
        //}

        // completionKey就是iocp_context_t指针（在aeApiAddEventEx中设置）
        // overlapped是iocp_context_t中的OVERLAPPED成员

        if (result > 0 && overlapped != NULL) {
            // 将传输的字节数写入到OVERLAPPED+int的位置
            int* bytesTransferredPtr = (int*)((char*)overlapped + sizeof(OVERLAPPED));
            *bytesTransferredPtr = (int)bytesTransferred;

            // 设置触发的事件
            // 这里我们简化处理，根据操作类型设置相应的事件掩码
            // 实际应该根据具体的操作类型来判断
            int mask = 0;

            // 检查是否是Accept操作
            SOCKET* acceptSocketPtr = (SOCKET*)((char*)overlapped + sizeof(OVERLAPPED));
            if (*acceptSocketPtr != INVALID_SOCKET && bytesTransferred > 0) {
                mask = AE_ACCEPT;

                // 设置接受socket选项
                SOCKET listenSocket = (SOCKET)completionKey; // completionKey是监听socket的上下文
                setsockopt(*acceptSocketPtr, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                    (char*)&listenSocket, sizeof(listenSocket));

                // 重置acceptSocket
                *acceptSocketPtr = INVALID_SOCKET;
            }
            else {
                mask = AE_READABLE;
            }

            if (mask != 0) {
                // fd存储在completionKey指向的iocp_context_t中，具体位置由上层决定
                // 这里我们假设completionKey就是iocp_context_t，fd在固定偏移位置
                int* fdPtr = (int*)((char*)completionKey + sizeof(OVERLAPPED) + sizeof(int) + sizeof(int));
                eventLoop->fired[numevents].fd = *fdPtr;
                eventLoop->fired[numevents].mask = mask;
                numevents++;
            }
        }
    }

    return numevents;
}

static char* aeApiName(void) {
    return "iocp";
}
