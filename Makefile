CC = gcc
CXX = g++

# 检测是否安装了clang++
ifneq ($(shell which clang++ > /dev/null 2>&1; echo $$?),0)
    # clang++ 不可用，使用默认的g++
    $(info Using g++ as compiler)
else
    # clang++ 可用，设置为默认编译器
    $(info Using clang++ as compiler)
    CC = clang
    CXX = clang++
endif

# C++编译标志 - 根据编译器类型调整协程标志
CXXFLAGS = -Wall -Wunused-function -g -std=c++20 -I .

# 根据编译器类型设置协程标志
ifneq ($(CXX),clang++)
    # GCC的协程标志
    CXXFLAGS += -fcoroutines -D__cpp_coroutines=201902L
else
    # Clang的协程标志
    CXXFLAGS += -fcoroutines-ts -D__cpp_coroutines=201902L
endif

CFLAGS = -Wall -Wextra -I .

# 添加必要的系统头文件路径和定义
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
    CFLAGS += -D_GNU_SOURCE
    CXXFLAGS += -D_GNU_SOURCE
endif

# 定义链接选项变量
LDFLAGS =

# 判断操作系统并设置相应的链接选项
ifeq ($(OS),Windows_NT)
    LDFLAGS += -lws2_32
    TARGET_EXT = .exe
else
    LDFLAGS += -lpthread
    # 添加必要的系统库
    LDFLAGS += -lm
    # 检测系统类型并添加相应库
    UNAME_S := $(shell uname -s)
    ifeq ($(UNAME_S),Darwin)
        # macOS平台需要链接execinfo库来支持backtrace功能
        LDFLAGS += -lexecinfo
    endif
    ifeq ($(UNAME_S),Linux)
        # Linux平台可能需要额外的库
        LDFLAGS += -ldl
    endif
    TARGET_EXT =
endif

# 构建目录
BUILD_DIR = build
OBJS_DIR = $(BUILD_DIR)/objs

# 源文件 - 将C和C++文件分开
C_SRCS = ae.c anet.c zmalloc.c xlog.c
CPP_SRCS = xchannel.cpp xcoroutine.cpp xrpc.cpp xthread.cpp xchannel_pdu.cpp xhandle.cpp
SVR_SRCS = demo/xthread_aeweakup.cpp
CLI_SRCS = demo/xrpc_client.cpp
TEST_SRCS = demo/test_macos_exception.cpp

# 目标文件（在构建目录中）
C_OBJS = $(addprefix $(OBJS_DIR)/, $(C_SRCS:.c=.o))
CPP_OBJS = $(addprefix $(OBJS_DIR)/, $(CPP_SRCS:.cpp=.o))
SVR_OBJS = $(addprefix $(OBJS_DIR)/, $(SVR_SRCS:.cpp=.o))
CLI_OBJS = $(addprefix $(OBJS_DIR)/, $(CLI_SRCS:.cpp=.o))
TEST_OBJS = $(addprefix $(OBJS_DIR)/, $(TEST_SRCS:.cpp=.o))

# 合并所有对象文件
OBJS = $(C_OBJS) $(CPP_OBJS)

# 可执行文件输出目录
TARGET_DIR = bin

# 默认目标
all : $(TARGET_DIR)/svr$(TARGET_EXT) $(TARGET_DIR)/client$(TARGET_EXT) $(TARGET_DIR)/test_exception$(TARGET_EXT)

# 创建必要的目录
$(shell mkdir -p $(OBJS_DIR)/demo $(TARGET_DIR))

# 服务器程序（使用 CXX 链接以确保C++运行时正确链接）
$(TARGET_DIR)/svr$(TARGET_EXT) : $(OBJS) $(SVR_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

# 客户端程序（使用 CXX 链接）
$(TARGET_DIR)/client$(TARGET_EXT) : $(OBJS) $(CLI_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

# 测试程序（使用 CXX 链接）
$(TARGET_DIR)/test_exception$(TARGET_EXT) : $(OBJS) $(TEST_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

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

# 依赖关系生成（可选，用于自动处理头文件依赖）
DEPFLAGS = -MT $@ -MMD -MP -MF $(OBJS_DIR)/$*.d

# 包含依赖文件
-include $(OBJS:.o=.d)

# 清理目标
clean:
	rm -rf $(BUILD_DIR)
	rm -rf $(TARGET_DIR)

# 单独编译客户端的快捷目标
cli: $(TARGET_DIR)/client$(TARGET_EXT)

# 单独编译服务器的快捷目标
svr: $(TARGET_DIR)/svr$(TARGET_EXT)

# 显示目录结构
tree:
	@echo "Build directory structure:"
	@find $(BUILD_DIR) -type f -o -type d | sort

# 调试信息目标
debug:
	@echo "Compiler: CC=$(CC), CXX=$(CXX)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "C_SRCS: $(C_SRCS)"
	@echo "CPP_SRCS: $(CPP_SRCS)"
	@echo "C_OBJS: $(C_OBJS)"
	@echo "CPP_OBJS: $(CPP_OBJS)"
	@echo "UNAME_S: $(UNAME_S)"

# 强制使用gcc/g++的目标
use-gcc:
	$(eval CC = gcc)
	$(eval CXX = g++)
	$(eval CXXFLAGS := $(filter-out -fcoroutines-ts,$(CXXFLAGS)))
	$(eval CXXFLAGS += -fcoroutines -D__cpp_coroutines=201902L)
	@echo "Forced using gcc/g++"

# 强制使用clang/clang++的目标
use-clang:
	$(eval CC = clang)
	$(eval CXX = clang++)
	$(eval CXXFLAGS := $(filter-out -fcoroutines,$(CXXFLAGS)))
	$(eval CXXFLAGS += -fcoroutines-ts -D__cpp_coroutines=201902L)
	@echo "Forced using clang/clang++"

# 安装依赖（示例，根据实际需要调整）
install-deps:
ifeq ($(UNAME_S),Linux)
	@echo "Installing build dependencies..."
	# 添加实际的包安装命令，例如：
	# sudo apt-get install build-essential clang
endif

.PHONY: all clean cli svr tree debug install-deps use-gcc use-clang
