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

# 系统自动检测
UNAME_S := $(shell uname -s)
ARCH := $(shell uname -m)
COMPILER := $(shell $(CXX) --version | head -n1)

# 自动检测编译器版本
CLANG_VERSION := $(shell $(CXX) --version 2>/dev/null | grep -i clang)
GCC_VERSION := $(shell $(CXX) --version 2>/dev/null | grep -i gcc)

# 自动识别编译器类型
ifneq ($(CLANG_VERSION),)
    IS_CLANG = 1
    COMPILER_NAME = clang
else ifneq ($(GCC_VERSION),)
    IS_GCC = 1
    COMPILER_NAME = gcc
else
    COMPILER_NAME = unknown
endif

# C 编译标志 - 通用
CFLAGS = -Wall -Wextra -I . -g -fno-omit-frame-pointer

# C++编译标志
CXXFLAGS = -Wall -Wunused-function -g -std=c++20 -I . -fno-omit-frame-pointer

# 平台特定的定义和标志
ifeq ($(UNAME_S),Linux)
    # Linux 平台
    CFLAGS += -D__linux__ -D_GNU_SOURCE
    CXXFLAGS += -D__linux__ -D_GNU_SOURCE
    PLATFORM = linux
    # Linux 需要 rdynamic 支持 backtrace
    ifeq ($(IS_CLANG),1)
        # Clang on Linux 也支持 -rdynamic
        CFLAGS += -rdynamic
        CXXFLAGS += -rdynamic
    else
        # GCC on Linux
        CFLAGS += -rdynamic
        CXXFLAGS += -rdynamic
    endif
else ifeq ($(UNAME_S),Darwin)
    # macOS 平台
    CFLAGS += -D__APPLE__ -D_DARWIN_C_SOURCE -D_DARWIN_UNLIMITED_SELECT
    CXXFLAGS += -D__APPLE__ -D_DARWIN_C_SOURCE -D_DARWIN_UNLIMITED_SELECT
    PLATFORM = darwin
    # macOS 不支持 -rdynamic，使用其他方式
    ifeq ($(IS_CLANG),1)
        # Clang on macOS
        CFLAGS += -mmacosx-version-min=10.14
        CXXFLAGS += -mmacosx-version-min=10.14
    endif
else ifeq ($(OS),Windows_NT)
    # Windows 平台
    CFLAGS += -D_WIN32 -D_WIN64
    CXXFLAGS += -D_WIN32 -D_WIN64
    PLATFORM = windows
else
    # 其他平台
    CFLAGS += -DPLATFORM_UNKNOWN
    CXXFLAGS += -DPLATFORM_UNKNOWN
    PLATFORM = unknown
endif

# 自动设置协程标志
ifeq ($(IS_CLANG),1)
    # Clang 协程标志
    CXXFLAGS += -fcoroutines-ts -D__cpp_coroutines=201902L
    # Clang 可能需要 libc++ 支持
    ifeq ($(UNAME_S),Darwin)
        CXXFLAGS += -stdlib=libc++
    endif
else
    # GCC 或其他编译器的协程标志
    CXXFLAGS += -fcoroutines -D__cpp_coroutines=201902L
endif

# 根据架构优化
ifeq ($(ARCH),x86_64)
    CFLAGS += -m64
    CXXFLAGS += -m64
else ifeq ($(ARCH),arm64)
    CFLAGS += -arch arm64
    CXXFLAGS += -arch arm64
    ifeq ($(UNAME_S),Darwin)
        CFLAGS += -target arm64-apple-darwin
        CXXFLAGS += -target arm64-apple-darwin
    endif
endif

# 定义链接选项变量
LDFLAGS =

# 自动设置链接选项
ifeq ($(PLATFORM),windows)
    LDFLAGS += -lws2_32
    TARGET_EXT = .exe
else
    LDFLAGS += -lpthread -lm
    
    # 根据平台自动设置链接选项
    ifeq ($(PLATFORM),linux)
        LDFLAGS += -ldl -rdynamic
        # 如果是 Linux，可能需要实时扩展
        ifneq ($(wildcard /usr/include/sys/eventfd.h),)
            CFLAGS += -DHAVE_EVENTFD
            CXXFLAGS += -DHAVE_EVENTFD
        endif
        ifneq ($(wildcard /usr/include/sys/epoll.h),)
            CFLAGS += -DHAVE_EPOLL
            CXXFLAGS += -DHAVE_EPOLL
        endif
    else ifeq ($(PLATFORM),darwin)
        # macOS 链接选项
        LDFLAGS += -undefined dynamic_lookup
        # 如果是 Clang，使用 libc++
        ifeq ($(IS_CLANG),1)
            LDFLAGS += -lc++
        endif
        # 检测 macOS 特定功能
        ifneq ($(wildcard /usr/include/sys/event.h),)
            CFLAGS += -DHAVE_KQUEUE
            CXXFLAGS += -DHAVE_KQUEUE
        endif
    endif
    
    TARGET_EXT =
endif

# 构建目录
BUILD_DIR = build
OBJS_DIR = $(BUILD_DIR)/objs

# 源文件 - 将C和C++文件分开
C_SRCS = ae.c anet.c zmalloc.c xlog.c xtimer.c
CPP_SRCS = xchannel.cpp xcoroutine.cpp xrpc.cpp xthread.cpp xchannel_pdu.cpp xhandle.cpp
SVR_SRCS = demo/xthread_demo.cpp
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

# 显示构建环境信息
$(info ========================================)
$(info Building for: $(UNAME_S)-$(ARCH))
$(info Platform: $(PLATFORM))
$(info Compiler: $(COMPILER_NAME) ($(shell $(CXX) --version | head -n1)))
$(info ========================================)

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
	@echo "=== Build Configuration ==="
	@echo "Platform: $(UNAME_S)-$(ARCH) [$(PLATFORM)]"
	@echo "Compiler: $(shell $(CXX) --version | head -n1)"
	@echo "Compiler Name: $(COMPILER_NAME)"
	@echo "CFLAGS: $(CFLAGS)"
	@echo "CXXFLAGS: $(CXXFLAGS)"
	@echo "LDFLAGS: $(LDFLAGS)"
	@echo "C_SRCS: $(C_SRCS)"
	@echo "CPP_SRCS: $(CPP_SRCS)"
	@echo "Is Clang: $(if $(IS_CLANG),Yes,No)"
	@echo "Is GCC: $(if $(IS_GCC),Yes,No)"

# 自动检测并修复编译环境
auto-setup:
ifeq ($(UNAME_S),Darwin)
	@echo "Setting up for macOS..."
	@if ! command -v xcode-select >/dev/null 2>&1; then \
		echo "Installing Xcode Command Line Tools..."; \
		xcode-select --install; \
	fi
	@if ! command -v brew >/dev/null 2>&1; then \
		echo "Homebrew not found. Consider installing it for package management."; \
	fi
else ifeq ($(UNAME_S),Linux)
	@echo "Setting up for Linux..."
	@if ! command -v apt-get >/dev/null 2>&1 && command -v yum >/dev/null 2>&1; then \
		echo "Detected yum-based system"; \
	elif command -v apt-get >/dev/null 2>&1; then \
		echo "Detected apt-based system"; \
	fi
endif

# 平台测试
platform-test:
	@echo "Running platform tests..."
	@echo "1. Testing architecture support..."
	@$(CC) -dM -E - < /dev/null | grep -i arch || echo "No architecture info"
	@echo "2. Testing system headers..."
	@$(CC) -xc - -o /dev/null 2>/dev/null <<<'#include <stdio.h>\nint main(){return 0;}' && echo "  - stdio.h: OK" || echo "  - stdio.h: FAILED"
	@echo "3. Testing C++20 support..."
	@$(CXX) -xc++ -std=c++20 - -o /dev/null 2>/dev/null <<<'int main(){return 0;}' && echo "  - C++20: OK" || echo "  - C++20: FAILED"

# 环境检查
env-check:
	@echo "=== Environment Check ==="
	@echo "OS: $(UNAME_S)"
	@echo "Architecture: $(ARCH)"
	@echo "Compiler: $(shell which $(CXX))"
	@echo "C Compiler: $(shell which $(CC))"
	@echo "Make version: $(shell make --version | head -n1)"
	@echo "Available memory: $(shell free -h 2>/dev/null | awk '/^Mem:/{print $2}' || sysctl -n hw.memsize 2>/dev/null | awk '{print int($1/1024/1024)"MB"}')"

# 构建所有平台配置
build-all: clean
	@echo "Building with current configuration..."
	@make
	@echo ""
	@echo "To build with different configurations, use:"
	@echo "  make use-gcc   # Force use GCC"
	@echo "  make use-clang # Force use Clang"

# 为 C 文件添加特定警告抑制规则（针对跨平台问题）
$(OBJS_DIR)/ae.o: CFLAGS += -Wno-void-pointer-to-int-cast -Wno-unused-parameter -Wno-conditional-type-mismatch
$(OBJS_DIR)/anet.o: CFLAGS += -Wno-unused-parameter

# 平台特定的修复规则
# 修复指针转换问题
FIX_POINTER_CONVERSION = sed -i.bak 's/(int)clientData/(int)(intptr_t)clientData/g' ae.c && \
                         sed -i.bak 's/clientData?clientData:fd/clientData ? clientData : (void*)(intptr_t)fd/g' ae.c

fix-pointer-conversion:
	@if [ -f ae.c ]; then \
		echo "Fixing pointer conversions in ae.c..."; \
		$(FIX_POINTER_CONVERSION); \
		rm -f ae.c.bak; \
		echo "Fixed."; \
	else \
		echo "ae.c not found."; \
	fi

.PHONY: all clean cli svr tree debug auto-setup platform-test env-check build-all fix-pointer-conversion
