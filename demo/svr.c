 /*Copyright (c) 2011, BohuTANG <overred.shuttler at gmail dot com>
 * All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include "anet.h"
#include "ae.h"

struct server {
	char *bindaddr;
	int port;
	xSocket fd;
	aeEventLoop *el;
	char neterr[1024];
};

struct server _svr;
static aeFileEvent* client_ev = NULL;
static aeFileEvent* accept_ev = NULL;

int _ReadHandler(aeEventLoop *el, xSocket fd, void *privdata, int mask, int data) {
	(void)el;
	(void)privdata;
	(void)mask;
	(void)data;
	char buf[1024];
	int nread = anetRead(fd, buf, 1024);
	
	if (nread <= 0) {
		if (nread < 0) {
			printf("Read error\n");
		} else {
			printf("Client disconnected\n");
		}
		aeDeleteFileEvent(_svr.el, fd, client_ev, AE_READABLE);
		anetCloseSocket(fd);
		return AE_OK;
	}

	buf[nread] = '\0';
	printf("Received: %s\n", buf);
	
	const char* response = "+OK\r\n";
	anetWrite(fd, response, strlen(response));
	return AE_OK;
}

int _AcceptHandler(aeEventLoop *el, xSocket fd, void *privdata, int mask, int data) {
	(void)el;
	(void)privdata;
	(void)mask;
	(void)data;
	printf("Accept connection on fd: %lld\n", (long long)fd);

	int cport;
	xSocket cfd;
	char cip[128];
	cfd = anetTcpAccept(_svr.neterr, fd, cip, &cport);
	if (cfd == ANET_ERR) {
		printf("Accept failed\n");
		return AE_OK;
	}

	printf("New client: %s:%d\n", cip, cport);
	aeCreateFileEvent(_svr.el, cfd, AE_READABLE, _ReadHandler, NULL, &client_ev);
	return AE_OK;
}

int main() {
	_svr.bindaddr = "127.0.0.1";
	_svr.port = 6379;

	_svr.el = aeCreateEventLoop(100);
	_svr.fd = anetTcpServer(_svr.neterr, _svr.port, _svr.bindaddr);
	if (_svr.fd == ANET_ERR) {
		printf("Failed to create server: %s\n", _svr.neterr);
		return 1;
	}

	if (aeCreateFileEvent(_svr.el, _svr.fd, AE_READABLE, _AcceptHandler, NULL, &accept_ev) == AE_ERR) {
		printf("Failed to create file event\n");
		return 1;
	}

	printf("Server started on %s:%d\n", _svr.bindaddr, _svr.port);
	aeMain(_svr.el);
	
	printf("Server exiting\n");
	aeDeleteEventLoop(_svr.el);
	return 0;
}
