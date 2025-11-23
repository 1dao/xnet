CC = gcc
CXX = g++
CXXFLAGS = -Wall -Wunused-function -g -std=c++20 -fcoroutines -D__cpp_coroutines=201902L -Wall -Wextra -I .
CFLAGS = -Wall -Wextra -I .
# 定义链接选项变量，默认为空
LDFLAGS = 

# 判断是否为 Windows 系统，是的话添加 -lws2_32
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
else
	LDFLAGS += -lpthread
endif

# 构建目录
BUILD_DIR = build
OBJS_DIR = $(BUILD_DIR)/objs

# 源文件
SRCS = ae.c anet.c request.c response.c zmalloc.c xchannel.c xlog.c
SVR_SRCS = demo/xnet_svr_iocp.c
CLI_SRCS = coroutine.cpp demo/xnet_client_iocp.cpp # 改为 .cpp  

# 目标文件（在构建目录中）
OBJS = $(addprefix $(OBJS_DIR)/, $(SRCS:.c=.o))
SVR_OBJS = $(addprefix $(OBJS_DIR)/, $(SVR_SRCS:.c=.o))
CLI_OBJS = $(addprefix $(OBJS_DIR)/, $(CLI_SRCS:.cpp=.o))  # 改为 .cpp=.o

# 可执行文件输出目录
TARGET_DIR = bin

all : $(TARGET_DIR)/svr $(TARGET_DIR)/client

# 创建必要的目录
$(shell mkdir -p $(OBJS_DIR)/demo $(TARGET_DIR))

# 服务器程序（使用 gcc 链接）
$(TARGET_DIR)/svr : $(OBJS) $(SVR_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# 客户端程序（使用 g++ 链接，因为包含 C++ 代码）
$(TARGET_DIR)/client : $(OBJS) $(CLI_OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

# C 文件编译规则
$(OBJS_DIR)/%.o : %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

# demo 目录下的 C 文件编译规则
$(OBJS_DIR)/demo/%.o : demo/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ -c $<

# C++ 文件编译规则
$(OBJS_DIR)/%.o : %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ -c $<

# demo 目录下的 C++ 文件编译规则
$(OBJS_DIR)/demo/%.o : demo/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ -c $<

clean :
	rm -rf $(BUILD_DIR)
	rm -rf bin

# 单独编译客户端的快捷目标
cli: $(TARGET_DIR)/client

# 单独编译服务器的快捷目标  
svr: $(TARGET_DIR)/svr

# 显示目录结构
tree:
	@echo "Build directory structure:"
	@find $(BUILD_DIR) -type f -o -type d | sort

.PHONY: all clean cli svr tree
