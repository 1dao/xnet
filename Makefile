CC = gcc
CFLAGS = -Wall -Wunused-function -g -std=c99 -D HAVE_EPOLL -g -I . 

# 定义链接选项变量，默认为空
LDFLAGS =

# 判断是否为 Windows 系统，是的话添加 -lws2_32
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
endif

# 构建目录
BUILD_DIR = build
OBJS_DIR = $(BUILD_DIR)/objs

# 源文件
SRCS = ae.c anet.c request.c response.c zmalloc.c achannel.c
SVR_SRCS = demo/xnet_svr_iocp.c
CLI_SRCS = demo/xnet_client.c

# 目标文件（在构建目录中）
OBJS = $(addprefix $(OBJS_DIR)/, $(SRCS:.c=.o))
SVR_OBJS = $(addprefix $(OBJS_DIR)/, $(SVR_SRCS:.c=.o))
CLI_OBJS = $(addprefix $(OBJS_DIR)/, $(CLI_SRCS:.c=.o))

# 可执行文件输出目录
TARGET_DIR = $(BUILD_DIR)/bin

all : $(TARGET_DIR)/svr $(TARGET_DIR)/client

# 创建必要的目录
$(shell mkdir -p $(OBJS_DIR)/demo $(TARGET_DIR))

# 服务器程序
$(TARGET_DIR)/svr : $(OBJS) $(SVR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 客户端程序
$(TARGET_DIR)/client : $(OBJS) $(CLI_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 编译规则：将所有 .c 文件编译到构建目录
$(OBJS_DIR)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

# 编译 demo 目录下的文件
$(OBJS_DIR)/demo/%.o : demo/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

clean :
	rm -rf $(BUILD_DIR)

# 单独编译客户端的快捷目标
client-only: $(TARGET_DIR)/client

# 单独编译服务器的快捷目标  
svr-only: $(TARGET_DIR)/svr

# 显示目录结构
tree:
	@echo "Build directory structure:"
	@find $(BUILD_DIR) -type f -o -type d | sort

.PHONY: all clean client-only svr-only tree
