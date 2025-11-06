/* anet.c -- Basic TCP socket stuff made a bit less boring
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

#include "fmacros.h"

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")  // 链接 Winsock 库

#else
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <sys/stat.h>
    #include <sys/un.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <netdb.h>
    #include <errno.h>
#endif

#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include "anet.h"

// Windows 下缺少 ssize_t 定义，手动补充
// #ifdef _WIN32
// typedef int ssize_t;
// #endif

static void anetSetError(char *err, const char *fmt, ...)
{
    va_list ap;

    if (!err) return;
    va_start(ap, fmt);
    vsnprintf(err, ANET_ERR_LEN, fmt, ap);
    va_end(ap);
}

// Windows 下获取错误信息字符串
static const char* anetStrError(int code, char *errbuf, size_t errbuf_len)
{
#ifdef _WIN32
    // 使用 FormatMessage 获取 Windows 错误信息
    DWORD flags = FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    DWORD len = FormatMessageA(flags, NULL, code, 0, errbuf, (DWORD)errbuf_len, NULL);
    if (len == 0) {
        snprintf(errbuf, errbuf_len, "Unknown error %d", code);
        return errbuf;
    }
    // 移除结尾的换行符
    while (len > 0 && (errbuf[len-1] == '\r' || errbuf[len-1] == '\n')) {
        len--;
    }
    errbuf[len] = '\0';
    return errbuf;
#else
    return strerror(code);
#endif
}

// 初始化 Winsock（仅 Windows 需要）
static int anetWSAInit(char *err)
{
#ifdef _WIN32
    WSADATA wsaData;
    int ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0) {
        anetSetError(err, "WSAStartup failed: %d", ret);
        return ANET_ERR;
    }
#endif
    return ANET_OK;
}

int anetNonBlock(char *err, int fd)
{
#ifdef _WIN32

    // Windows 使用 ioctlsocket 设置非阻塞模式
    u_long mode = 1;  // 1 表示非阻塞，0 表示阻塞
    if (ioctlsocket(fd, FIONBIO, &mode) == SOCKET_ERROR) {
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "ioctlsocket(FIONBIO): %s", 
                    anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        return ANET_ERR;
    }
#else
    // POSIX 系统使用 fcntl
    int flags;
    if ((flags = fcntl(fd, F_GETFL)) == -1) {
        anetSetError(err, "fcntl(F_GETFL): %s", strerror(errno));
        return ANET_ERR;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        anetSetError(err, "fcntl(F_SETFL,O_NONBLOCK): %s", strerror(errno));
        return ANET_ERR;
    }
#endif
    return ANET_OK;
}

int anetTcpNoDelay(char *err, int fd)
{
    int yes = 1;
#ifdef _WIN32
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof(yes)) == SOCKET_ERROR) {
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "setsockopt TCP_NODELAY: %s",
                    anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        return ANET_ERR;
    }
#else
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt TCP_NODELAY: %s", strerror(errno));
        return ANET_ERR;
    }
#endif
    return ANET_OK;
}

int anetSetSendBuffer(char *err, int fd, int buffsize)
{
#ifdef _WIN32
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char*)&buffsize, sizeof(buffsize)) == SOCKET_ERROR) {
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "setsockopt SO_SNDBUF: %s",
                    anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        return ANET_ERR;
    }
#else
    if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffsize, sizeof(buffsize)) == -1) {
        anetSetError(err, "setsockopt SO_SNDBUF: %s", strerror(errno));
        return ANET_ERR;
    }
#endif
    return ANET_OK;
}

int anetTcpKeepAlive(char *err, int fd)
{
    int yes = 1;
#ifdef _WIN32
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&yes, sizeof(yes)) == SOCKET_ERROR) {
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s",
                    anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        return ANET_ERR;
    }
#else
    if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes)) == -1) {
        anetSetError(err, "setsockopt SO_KEEPALIVE: %s", strerror(errno));
        return ANET_ERR;
    }
#endif
    return ANET_OK;
}

int anetResolve(char *err, char *host, char *ipbuf)
{
    struct sockaddr_in sa;
    sa.sin_family = AF_INET;

#ifdef _WIN32
    // Windows 推荐使用 inet_pton 替代 inet_aton（后者已过时）
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1) {
#else
    if (inet_aton(host, &sa.sin_addr) == 0) {
#endif
        struct hostent *he;
#ifdef _WIN32
        he = gethostbyname(host);
#else
        he = gethostbyname(host);
#endif
        if (he == NULL) {
            anetSetError(err, "can't resolve: %s", host);
            return ANET_ERR;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
    }
#ifdef _WIN32
    inet_ntop(AF_INET, &sa.sin_addr, ipbuf, INET_ADDRSTRLEN);
#else
    strcpy(ipbuf, inet_ntoa(sa.sin_addr));
#endif
    return ANET_OK;
}

static int anetCreateSocket(char *err, int domain)
{
    // 初始化 Winsock（仅首次调用时生效）
    static int wsa_inited = 0;
    if (!wsa_inited) {
        if (anetWSAInit(err) != ANET_OK) {
            return ANET_ERR;
        }
        wsa_inited = 1;
    }

    int s, on = 1;
#ifdef _WIN32
    s = socket(domain, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) {
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "creating socket: %s",
                    anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        return ANET_ERR;
    }
#else
    if ((s = socket(domain, SOCK_STREAM, 0)) == -1) {
        anetSetError(err, "creating socket: %s", strerror(errno));
        return ANET_ERR;
    }
#endif

    // 设置 SO_REUSEADDR 选项
#ifdef _WIN32
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&on, sizeof(on)) == SOCKET_ERROR) {
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "setsockopt SO_REUSEADDR: %s",
                    anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        closesocket(s);
        return ANET_ERR;
    }
#else
    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
        anetSetError(err, "setsockopt SO_REUSEADDR: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
#endif
    return s;
}

#define ANET_CONNECT_NONE 0
#define ANET_CONNECT_NONBLOCK 1
static int anetTcpGenericConnect(char *err, char *addr, int port, int flags)
{
    int s;
    struct sockaddr_in sa;

    if ((s = anetCreateSocket(err, AF_INET)) == ANET_ERR)
        return ANET_ERR;

    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
#ifdef _WIN32
    if (inet_pton(AF_INET, addr, &sa.sin_addr) != 1) {
#else
    if (inet_aton(addr, &sa.sin_addr) == 0) {
#endif
        struct hostent *he;
        he = gethostbyname(addr);
        if (he == NULL) {
            anetSetError(err, "can't resolve: %s", addr);
#ifdef _WIN32
            closesocket(s);
#else
            close(s);
#endif
            return ANET_ERR;
        }
        memcpy(&sa.sin_addr, he->h_addr, sizeof(struct in_addr));
    }

    if (flags & ANET_CONNECT_NONBLOCK) {
        if (anetNonBlock(err, s) != ANET_OK) {
#ifdef _WIN32
            closesocket(s);
#else
            close(s);
#endif
            return ANET_ERR;
        }
    }

#ifdef _WIN32
    int ret = connect(s, (struct sockaddr*)&sa, sizeof(sa));
    if (ret == SOCKET_ERROR) {
        int err_code = WSAGetLastError();
        if (err_code == WSAEINPROGRESS && (flags & ANET_CONNECT_NONBLOCK)) {
            return s;  // 非阻塞模式下连接正在进行
        }
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "connect: %s", anetStrError(err_code, errbuf, sizeof(errbuf)));
        closesocket(s);
        return ANET_ERR;
    }
#else
    if (connect(s, (struct sockaddr*)&sa, sizeof(sa)) == -1) {
        if (errno == EINPROGRESS && (flags & ANET_CONNECT_NONBLOCK))
            return s;
        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
#endif
    return s;
}

int anetTcpConnect(char *err, char *addr, int port)
{
    return anetTcpGenericConnect(err,addr,port,ANET_CONNECT_NONE);
}

int anetTcpNonBlockConnect(char *err, char *addr, int port)
{
    return anetTcpGenericConnect(err,addr,port,ANET_CONNECT_NONBLOCK);
}

int anetUnixGenericConnect(char *err, char *path, int flags)
{
#ifndef _WIN32
    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (flags & ANET_CONNECT_NONBLOCK) {
        if (anetNonBlock(err,s) != ANET_OK)
            return ANET_ERR;
    }
    if (connect(s,(struct sockaddr*)&sa,sizeof(sa)) == -1) {
        if (errno == EINPROGRESS &&
            flags & ANET_CONNECT_NONBLOCK)
            return s;

        anetSetError(err, "connect: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    return s;
#else
    return ANET_ERR;
#endif
}

int anetUnixConnect(char *err, char *path)
{
    return anetUnixGenericConnect(err,path,ANET_CONNECT_NONE);
}

int anetUnixNonBlockConnect(char *err, char *path)
{
    return anetUnixGenericConnect(err,path,ANET_CONNECT_NONBLOCK);
}

/* Like read(2) but make sure 'count' is read before to return
 * (unless error or EOF condition is encountered) */
int anetRead(int fd, char *buf, int count)
{
    int nread, totlen = 0;
    while (totlen != count) {
#ifdef _WIN32
        nread = recv(fd, buf, count - totlen, 0);
#else
        nread = read(fd, buf, count - totlen);
#endif
        if (nread == 0) return totlen;  // 连接关闭
        if (nread == -1) {
#ifdef _WIN32
            int err_code = WSAGetLastError();
            if (err_code == WSAEWOULDBLOCK || err_code == WSAEINTR) {
                continue;  // 非阻塞模式下重试
            }
#endif
            return -1;  // 错误
        }
        totlen += nread;
        buf += nread;
    }
    return totlen;
}


/* Like write(2) but make sure 'count' is read before to return
 * (unless error is encountered) */
int anetWrite(int fd, char *buf, int count)
{
    int nwritten, totlen = 0;
    while (totlen != count) {
#ifdef _WIN32
        nwritten = send(fd, buf, count - totlen, 0);
#else
        nwritten = write(fd, buf, count - totlen);
#endif
        if (nwritten == 0) return totlen;
        if (nwritten == -1) {
#ifdef _WIN32
            int err_code = WSAGetLastError();
            if (err_code == WSAEWOULDBLOCK || err_code == WSAEINTR) {
                continue;  // 非阻塞模式下重试
            }
#endif
            return -1;
        }
        totlen += nwritten;
        buf += nwritten;
    }
    return totlen;
}

static int anetListen(char *err, int s, struct sockaddr *sa, socklen_t len)
{
#ifdef _WIN32
    if (bind(s, sa, len) == SOCKET_ERROR) {
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "bind: %s", anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        closesocket(s);
        return ANET_ERR;
    }
    if (listen(s, 511) == SOCKET_ERROR) {  // 511 为最大监听队列长度
        char errbuf[ANET_ERR_LEN];
        anetSetError(err, "listen: %s", anetStrError(WSAGetLastError(), errbuf, sizeof(errbuf)));
        closesocket(s);
        return ANET_ERR;
    }
#else
    if (bind(s, sa, len) == -1) {
        anetSetError(err, "bind: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
    if (listen(s, 511) == -1) {
        anetSetError(err, "listen: %s", strerror(errno));
        close(s);
        return ANET_ERR;
    }
#endif
    return ANET_OK;
}

int anetTcpServer(char *err, int port, char *bindaddr)
{
    int s;
    struct sockaddr_in sa;

    if ((s = anetCreateSocket(err, AF_INET)) == ANET_ERR)
        return ANET_ERR;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);  // 绑定所有网卡

    if (bindaddr) {
#ifdef _WIN32
        if (inet_pton(AF_INET, bindaddr, &sa.sin_addr) != 1) {
#else
        if (inet_aton(bindaddr, &sa.sin_addr) == 0) {
#endif
            anetSetError(err, "invalid bind address");
#ifdef _WIN32
            closesocket(s);
#else
            close(s);
#endif
            return ANET_ERR;
        }
    }

    if (anetListen(err, s, (struct sockaddr*)&sa, sizeof(sa)) == ANET_ERR) {
#ifdef _WIN32
        closesocket(s);
#else
        close(s);
#endif
        return ANET_ERR;
    }
    return s;
}

/*
int anetUnixServer(char *err, char *path, mode_t perm)
{
    int s;
    struct sockaddr_un sa;

    if ((s = anetCreateSocket(err,AF_LOCAL)) == ANET_ERR)
        return ANET_ERR;

    memset(&sa,0,sizeof(sa));
    sa.sun_family = AF_LOCAL;
    strncpy(sa.sun_path,path,sizeof(sa.sun_path)-1);
    if (anetListen(err,s,(struct sockaddr*)&sa,sizeof(sa)) == ANET_ERR)
        return ANET_ERR;
    if (perm)
        chmod(sa.sun_path, perm);
    return s;
}*/

static int anetGenericAccept(char *err, int s, struct sockaddr *sa, socklen_t *len)
{
    int fd;
    while (1) {
#ifdef _WIN32
        fd = accept(s, sa, len);
        if (fd == INVALID_SOCKET) {
            int err_code = WSAGetLastError();
            if (err_code == WSAEINTR)
                continue;  // 被信号中断，重试
            char errbuf[ANET_ERR_LEN];
            anetSetError(err, "accept: %s", anetStrError(err_code, errbuf, sizeof(errbuf)));
            return ANET_ERR;
        }
#else
        fd = accept(s, sa, len);
        if (fd == -1) {
            if (errno == EINTR)
                continue;
            anetSetError(err, "accept: %s", strerror(errno));
            return ANET_ERR;
        }
#endif
        break;
    }
    return fd;
}

int anetTcpAccept(char *err, int s, char *ip, int *port)
{
    int fd;
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err, s, (struct sockaddr*)&sa, &salen)) == ANET_ERR)
        return ANET_ERR;

    if (ip) {
#ifdef _WIN32
        inet_ntop(AF_INET, &sa.sin_addr, ip, INET_ADDRSTRLEN);
#else
        strcpy(ip, inet_ntoa(sa.sin_addr));
#endif
    }
    if (port) *port = ntohs(sa.sin_port);
    return fd;
}

int anetUnixAccept(char *err, int s) {
#ifdef _WIN32
    return -1;
#else
    int fd;
    struct sockaddr_un sa;
    socklen_t salen = sizeof(sa);
    if ((fd = anetGenericAccept(err,s,(struct sockaddr*)&sa,&salen)) == ANET_ERR)
        return ANET_ERR;

    return fd;
#endif
}
int anetPeerToString(int fd, char *ip, int *port)
{
    struct sockaddr_in sa;
    socklen_t salen = sizeof(sa);

#ifdef _WIN32
    if (getpeername(fd, (struct sockaddr*)&sa, &salen) == SOCKET_ERROR)
        return -1;
#else
    if (getpeername(fd, (struct sockaddr*)&sa, &salen) == -1)
        return -1;
#endif

    if (ip) {
#ifdef _WIN32
        inet_ntop(AF_INET, &sa.sin_addr, ip, INET_ADDRSTRLEN);
#else
        strcpy(ip, inet_ntoa(sa.sin_addr));
#endif
    }
    if (port) *port = ntohs(sa.sin_port);
    return 0;
}

// Windows 下关闭套接字的封装
#ifdef _WIN32
#define anetCloseSocket closesocket
#else
#define anetCloseSocket close
#endif
