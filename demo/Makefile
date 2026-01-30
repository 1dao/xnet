# Demo Makefile - Build all example programs
# Supports Windows(MSYS2/MinGW), Linux and macOS
# Each demo only compiles its required dependencies

# Compiler settings
CC = gcc
CXX = g++

# Build mode: debug or release
BUILD ?= debug

# Detect platform
UNAME_S := $(shell uname -s)

# Base LDFLAGS (Release mode uses dynamic linking for smaller size)
ifeq ($(BUILD),release)
    OPT_FLAGS = -O2 -s
    DEBUG_FLAGS =
    ifeq ($(OS),Windows_NT)
        LDFLAGS_BASE = -lws2_32 -lpthread -ldbghelp
    else
        LDFLAGS_BASE = -lpthread -lm
    endif
else
    OPT_FLAGS = -O0
    DEBUG_FLAGS = -g
    ifeq ($(OS),Windows_NT)
        LDFLAGS_BASE = -lws2_32 -lpthread -ldbghelp
    else
        LDFLAGS_BASE = -lpthread -lm
    endif
endif

# Platform specific settings
ifeq ($(UNAME_S),Linux)
    PLATFORM = linux
    CFLAGS = -Wall -Wextra -I.. $(OPT_FLAGS) $(DEBUG_FLAGS) -D__linux__ -D_GNU_SOURCE
    CXXFLAGS = -Wall -Wextra -std=c++20 -I.. $(OPT_FLAGS) $(DEBUG_FLAGS) -D__linux__ -D_GNU_SOURCE
    LDFLAGS = $(LDFLAGS_BASE) -ldl -rdynamic
    TARGET_EXT =
else ifeq ($(UNAME_S),Darwin)
    PLATFORM = darwin
    CFLAGS = -Wall -Wextra -I.. $(OPT_FLAGS) $(DEBUG_FLAGS) -D__APPLE__ -D_DARWIN_C_SOURCE
    CXXFLAGS = -Wall -Wextra -std=c++20 -I.. $(OPT_FLAGS) $(DEBUG_FLAGS) -D__APPLE__ -D_DARWIN_C_SOURCE
    LDFLAGS = $(LDFLAGS_BASE)
    TARGET_EXT =
else ifeq ($(OS),Windows_NT)
    PLATFORM = windows
    CFLAGS = -Wall -Wextra -I.. $(OPT_FLAGS) $(DEBUG_FLAGS) -D_WIN32 -D_WIN64
    CXXFLAGS = -Wall -Wextra -std=c++20 -I.. $(OPT_FLAGS) $(DEBUG_FLAGS) -D_WIN32 -D_WIN64
    LDFLAGS = $(LDFLAGS_BASE)
    TARGET_EXT = .exe
else
    PLATFORM = unknown
    CFLAGS = -Wall -Wextra -I.. $(OPT_FLAGS) $(DEBUG_FLAGS)
    CXXFLAGS = -Wall -Wextra -std=c++20 -I.. $(OPT_FLAGS) $(DEBUG_FLAGS)
    LDFLAGS = $(LDFLAGS_BASE)
    TARGET_EXT =
endif

# Directory settings
BIN_DIR = ../bin
OBJ_DIR = .obj
XNET_OBJ_DIR = $(OBJ_DIR)/xnet

# Create directories
$(shell mkdir -p $(BIN_DIR) $(OBJ_DIR) $(XNET_OBJ_DIR))

# ============================================
# Demo and dependencies (use filenames directly)
# ============================================

# xhttpd_svr - HTTP server demo
xhttpd_svr_SRC = xhttpd_svr.cpp
xhttpd_svr_DEPS = \
    ../3rd/picohttpparser.c \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xhttpd.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xrpc_server / xrpc_client - RPC demo
xrpc_server_SRC = xrpc_server.cpp
xrpc_server_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

xrpc_client_SRC = xrpc_client.cpp
xrpc_client_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xthread_demo - Thread pool demo
xthread_demo_SRC = xthread_demo.cpp
xthread_demo_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xnet_client / xnet_svr - Basic networking demo
# xnet_client is simple TCP client, doesn't need xhandle/xrpc/xchannel
xnet_client_SRC = xnet_client.cpp
xnet_client_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c

xnet_svr_SRC = xnet_svr.c
xnet_svr_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c

# xnet_client_coroutine - Coroutine client
xnet_client_coroutine_SRC = xnet_client_coroutine.cpp
xnet_client_coroutine_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xnet_svr_coroutine - Coroutine server
xnet_svr_coroutine_SRC = xnet_svr_coroutine.cpp
xnet_svr_coroutine_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xnet_svr_iocp - Windows IOCP server
xnet_svr_iocp_SRC = xnet_svr_iocp.c
xnet_svr_iocp_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xnet_coroutine - Coroutine demo
xnet_coroutine_SRC = xnet_coroutine.cpp
xnet_coroutine_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xpac_server - Packet server
xpac_server_SRC = xpac_server.cpp
xpac_server_DEPS = \
    ../3rd/picohttpparser.c \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xhttpd.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xredis_client - Redis client
xredis_client_SRC = xredis_client.cpp
xredis_client_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xredis.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# xcoroutine_exception - Coroutine exception handling demo
xcoroutine_exception_SRC = xcoroutine_exception.cpp
xcoroutine_exception_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xcoroutine.cpp \
    ../xthread.cpp

# xthread_aeweakup - Event loop wakeup demo
xthread_aeweakup_SRC = xthread_aeweakup.cpp
xthread_aeweakup_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c \
    ../xchannel.cpp \
    ../xchannel_pdu.cpp \
    ../xcoroutine.cpp \
    ../xhandle.cpp \
    ../xrpc.cpp \
    ../xthread.cpp

# svr - Basic server
svr_SRC = svr.c
svr_DEPS = \
    ../ae.c \
    ../anet.c \
    ../zmalloc.c \
    ../xlog.c \
    ../xtimer.c

# ============================================
# All demos list
# ============================================

DEMOS = xhttpd_svr xrpc_server xrpc_client xthread_demo xnet_client xnet_svr \
        xnet_client_coroutine xnet_svr_coroutine xnet_svr_iocp xnet_coroutine \
        xpac_server xredis_client xcoroutine_exception xthread_aeweakup svr

# Generate all targets
ALL_TARGETS = $(patsubst %,$(BIN_DIR)/%$(TARGET_EXT),$(DEMOS))

# ============================================
# Build rules
# ============================================

# Disable built-in suffix rules
.SUFFIXES:

.PHONY: all clean help list

# Default target: build all demos
all: $(ALL_TARGETS)
	@echo "========================================"
	@echo "All demos built successfully!"
	@echo "Output: $(BIN_DIR)"
	@echo "========================================"
	@rm -rf $(OBJ_DIR)
	@echo "Cleaned intermediate files (.obj)"

# Help
help:
	@echo "========================================"
	@echo "Demo Makefile Usage"
	@echo "========================================"
	@echo "Targets:"
	@echo "  make all          - Build all demos (auto delete .obj)"
	@echo "  make list         - List all available demos"
	@echo "  make clean        - Clean all build artifacts"
	@echo "  make <demo_name>  - Build specific demo (auto delete .obj)"
	@echo ""
	@echo "Build modes:"
	@echo "  make BUILD=debug <target>   - Debug mode (default, with symbols)"
	@echo "  make BUILD=release <target> - Release mode (optimized, smaller)"
	@echo ""
	@echo "Examples:"
	@echo "  make xhttpd_svr              # Debug build"
	@echo "  make BUILD=release xhttpd_svr # Release build (~10x smaller)"
	@echo "  make BUILD=release all       # Release build all"
	@echo "========================================"

# List available demos
list:
	@echo "========================================"
	@echo "Available demos:"
	@echo "========================================"
	@for demo in $(DEMOS); do \
		echo "  - $$demo"; \
	done
	@echo "========================================"

# Clean
clean:
	@rm -rf $(OBJ_DIR)
	@rm -f $(ALL_TARGETS)
	@echo "Cleaned!"

# ============================================
# General compilation rules
# ============================================

# xnet C files (../xxx.c -> .obj/xnet/xxx.o)
$(XNET_OBJ_DIR)/%.o: ../%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# xnet C++ files (../xxx.cpp -> .obj/xnet/xxx.o)
$(XNET_OBJ_DIR)/%.o: ../%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# 3rd party files (../3rd/)
$(XNET_OBJ_DIR)/3rd/%.o: ../3rd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Demo 3rd party (demo/3rd/ -> .obj/3rd/)
$(OBJ_DIR)/3rd/%.o: 3rd/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Demo C files (xxx.c -> .obj/xxx.o)
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# Demo C++ files (xxx.cpp -> .obj/xxx.o)
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# xnet_svr_iocp uses C++ compiler (needs xchannel.cpp)
$(OBJ_DIR)/xnet_svr_iocp.o: xnet_svr_iocp.c
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# ============================================
# Generic linking pattern
# ============================================

# Convert dependency paths to object paths
define LINK_DEMO
$(BIN_DIR)/$(1)$(TARGET_EXT): $(OBJ_DIR)/$(1).o $$(patsubst ../%.c,$(XNET_OBJ_DIR)/%.o,$$(patsubst ../%.cpp,$(XNET_OBJ_DIR)/%.o,$(2)))
	$(CXX) -o $$@ $$^ $(LDFLAGS)
	@echo "[OK] $(1) built!"
endef

# Generate link rules for each demo
$(foreach demo,$(DEMOS),$(eval $(call LINK_DEMO,$(demo),$($(demo)_DEPS))))

# ============================================
# Shortcut targets
# ============================================

xhttpd_svr: $(BIN_DIR)/xhttpd_svr$(TARGET_EXT)

xrpc_server: $(BIN_DIR)/xrpc_server$(TARGET_EXT)

xrpc_client: $(BIN_DIR)/xrpc_client$(TARGET_EXT)

xthread_demo: $(BIN_DIR)/xthread_demo$(TARGET_EXT)

xnet_client: $(BIN_DIR)/xnet_client$(TARGET_EXT)

xnet_svr: $(BIN_DIR)/xnet_svr$(TARGET_EXT)

xnet_client_coroutine: $(BIN_DIR)/xnet_client_coroutine$(TARGET_EXT)

xnet_svr_coroutine: $(BIN_DIR)/xnet_svr_coroutine$(TARGET_EXT)

xnet_svr_iocp: $(BIN_DIR)/xnet_svr_iocp$(TARGET_EXT)

xnet_coroutine: $(BIN_DIR)/xnet_coroutine$(TARGET_EXT)

xpac_server: $(BIN_DIR)/xpac_server$(TARGET_EXT)

xredis_client: $(BIN_DIR)/xredis_client$(TARGET_EXT)

xcoroutine_exception: $(BIN_DIR)/xcoroutine_exception$(TARGET_EXT)

xthread_aeweakup: $(BIN_DIR)/xthread_aeweakup$(TARGET_EXT)

svr: $(BIN_DIR)/svr$(TARGET_EXT)

# ============================================
# Info output
# ============================================
$(info ========================================)
$(info Demo Makefile for $(PLATFORM))
$(info Use 'make help' for usage)
$(info ========================================)
