#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ae.h"
#include "anet.h"

// Protocol handler function pointer type
typedef int (*ProtocolHandler)(int param1, const char* param2, int param2_len, char* response, int* response_len);

// Protocol structure
typedef struct {
    uint16_t protocol;
    ProtocolHandler handler;
} ProtocolMapping;

// Global protocol mapping table
static ProtocolMapping protocol_handlers[256];
static int handler_count = 0;

// Register protocol handler
void register_protocol_handler(uint16_t protocol, ProtocolHandler handler) {
    if (handler_count < 256) {
        protocol_handlers[handler_count].protocol = protocol;
        protocol_handlers[handler_count].handler = handler;
        handler_count++;
    }
}

// Find protocol handler
ProtocolHandler find_protocol_handler(uint16_t protocol) {
    for (int i = 0; i < handler_count; i++) {
        if (protocol_handlers[i].protocol == protocol) {
            return protocol_handlers[i].handler;
        }
    }
    return NULL;
}

// Example protocol handler 1
int handle_protocol_1(int param1, const char* param2, int param2_len, char* response, int* response_len) {
    printf("Processing protocol 1: param1=%d, param2=%.*s\n", param1, param2_len, param2);
    *response_len = sprintf(response, "Protocol 1 response: %d", param1 * 2);
    return 0; // Success
}

// Example protocol handler 2
int handle_protocol_2(int param1, const char* param2, int param2_len, char* response, int* response_len) {
    (void)param2;
    printf("Processing protocol 2: param1=%d, param2_len=%d\n", param1, param2_len);
    *response_len = sprintf(response, "Protocol 2 response: %d bytes received", param2_len);
    return 0; // Success
}

// Process incoming request
void process_request(const char* request, int request_len, char* response, int* response_len) {
    if (request_len < 12) { // Minimum header size: 4+2+1+1+4=12 bytes
        *response_len = 0;
        return;
    }

    // Parse request header
    uint32_t pkg_len = *(const uint32_t*)request;
    uint16_t protocol = *(const uint16_t*)(request + 4);
    uint8_t need_return = *(const uint8_t*)(request + 6);
    uint8_t is_request = *(const uint8_t*)(request + 7);
    uint32_t pkg_id = *(const uint32_t*)(request + 8);

    // Verify packet length
    if (pkg_len != (uint32_t)request_len) {
        printf("Packet length mismatch: %d vs %d\n", pkg_len, request_len);
        *response_len = 0;
        return;
    }

    // Check if it's a request packet
    if (is_request != 1) {
        printf("Not a request packet\n");
        *response_len = 0;
        return;
    }

    // Parse parameters
    int param1 = 0;
    const char* param2 = NULL;
    int param2_len = 0;

    if (request_len > 12) {
        param1 = *(const int*)(request + 12);
        if (request_len > 16) {
            param2 = request + 16;
            param2_len = request_len - 16;
        }
    }

    // Find and call protocol handler
    ProtocolHandler handler = find_protocol_handler(protocol);
    char handler_response[1024] = { 0 };
    int handler_response_len = 0;

    if (handler) {
        (void)handler(param1, param2, param2_len, handler_response, &handler_response_len);
    }
    else {
        printf("No handler found for protocol %d\n", protocol);
        *response_len = 0;
        return;
    }

    // Build response packet
    if (need_return) {
        // Response header size: 4+2+1+1+4=12 bytes
        *response_len = 12 + handler_response_len;
        uint32_t resp_pkg_len = (uint32_t)*response_len;
        uint8_t return_flag = 0; // No return
        uint8_t is_response = 0; // Response packet

        memcpy(response, &resp_pkg_len, 4);
        memcpy(response + 4, &protocol, 2);
        memcpy(response + 6, &return_flag, 1);
        memcpy(response + 7, &is_response, 1);
        memcpy(response + 8, &pkg_id, 4);
        memcpy(response + 12, handler_response, handler_response_len);
    }
    else {
        *response_len = 0;
    }
}

// Client data read handler
static aeFileEvent* client_ev = NULL;

int read_handler(aeEventLoop* el, xSocket fd, void* privdata, int mask, int data) {
    (void)privdata;
    (void)mask;
    (void)data;
    char buf[4096];
    int nread = anetRead(fd, buf, sizeof(buf));
    if (nread <= 0) {
        if (nread < 0) {
            printf("Read error\n");
        } else {
            printf("Client disconnected\n");
        }
        aeDeleteFileEvent(el, fd, client_ev, AE_READABLE);
        anetCloseSocket(fd);
        return AE_OK;
    }

    // Process request and send response
    char response[4096];
    int response_len = 0;
    process_request(buf, nread, response, &response_len);

    // Send response
    if (response_len > 0) {
        anetWrite(fd, response, response_len);
    }
    return AE_OK;
}

// Accept connection handler
static aeFileEvent* server_ev = NULL;

int accept_handler(aeEventLoop* el, xSocket fd, void* privdata, int mask, int data) {
    (void)privdata;
    (void)mask;
    (void)data;
    char ip[64];
    int port;
    xSocket client_fd = anetTcpAccept(NULL, fd, ip, &port);

    if (client_fd == ANET_ERR) {
        printf("Accept failed\n");
        return AE_OK;
    }

    printf("New connection: %s:%d\n", ip, port);

    // Set non-blocking mode
    anetNonBlock(NULL, client_fd);
    // Disable Nagle algorithm
    anetTcpNoDelay(NULL, client_fd);

    // Register read event
    if (aeCreateFileEvent(el, client_fd, AE_READABLE, read_handler, NULL, &client_ev) == AE_ERR) {
        printf("Register event failed\n");
        anetCloseSocket(client_fd);
    }
    return AE_OK;
}

int main(int argc, char* argv[]) {
    int port = 6379; // Default port
    if (argc > 1) {
        port = atoi(argv[1]);
    }

    // Register protocol handlers
    register_protocol_handler(1, handle_protocol_1);
    register_protocol_handler(2, handle_protocol_2);

    // Create event loop
    aeEventLoop* el = aeCreateEventLoop(1024);
    if (!el) {
        printf("Failed to create event loop\n");
        return 1;
    }

    // Create TCP server
    char err[ANET_ERR_LEN];
    xSocket server_fd = anetTcpServer(err, port, NULL);
    if (server_fd == ANET_ERR) {
        printf("Failed to create server: %s\n", err);
        return 1;
    }

    // Set non-blocking mode
    anetNonBlock(err, server_fd);

    // Register accept event
    if (aeCreateFileEvent(el, server_fd, AE_READABLE, accept_handler, NULL, &server_ev) == AE_ERR) {
        printf("Register accept event failed\n");
        return 1;
    }

    printf("Server started on port %d\n", port);
    aeMain(el);
    aeDeleteEventLoop(el);

    return 0;
}
