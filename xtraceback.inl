// xtraceback.inl - 跨平台堆栈跟踪实现
#ifndef XTRACEBACK_INL
#define XTRACEBACK_INL

#include "xlog.h"
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <atomic>

#if defined(_WIN32)
    #define XCORO_PLATFORM_WINDOWS 1
    #define XCORO_PLATFORM_UNIX 0
    #define XCORO_PLATFORM_APPLE 0
    #define XCORO_PLATFORM_LINUX 0
    #include <windows.h>
    #include <dbghelp.h>
    #include <atomic>
    #ifdef _MSC_VER
    #pragma comment(lib, "dbghelp.lib")
    #endif
#elif defined(__APPLE__)
    #define XCORO_PLATFORM_WINDOWS 0
    #define XCORO_PLATFORM_UNIX 1
    #define XCORO_PLATFORM_APPLE 1
    #define XCORO_PLATFORM_LINUX 0
    #include <dlfcn.h>
    #include <execinfo.h>
    #include <cxxabi.h>
    #include <mach-o/dyld.h>
    #include <mach-o/getsect.h>
    #include <cstring>
    #include <sys/stat.h>
    #include <unistd.h>
    #include <sys/ucontext.h>
#elif defined(__linux__)
    #define XCORO_PLATFORM_WINDOWS 0
    #define XCORO_PLATFORM_UNIX 1
    #define XCORO_PLATFORM_APPLE 0
    #define XCORO_PLATFORM_LINUX 1
    #include <dlfcn.h>
    #include <execinfo.h>
    #include <cxxabi.h>
    #include <link.h>
    #include <sys/stat.h>
    #include <unistd.h>
    #include <sys/ucontext.h>
#else
    #error "Unsupported platform"
#endif

// ==============================================
// 全局状态管理
// ==============================================

// 堆栈跟踪模式
enum xTracebackMode {
    XTB_MODE_UNINITIALIZED = 0,  // 未初始化
    XTB_MODE_SIMPLE = 1,         // 简化模式
    XTB_MODE_DETAILED = 2,       // 详细模式（使用外部工具）
    XTB_MODE_AUTO = 3            // 自动检测
};

// ==============================================
// 工具检测函数
// ==============================================
// 全局状态变量（线程安全的初始化）
static std::atomic<xTracebackMode> g_traceback_mode = XTB_MODE_UNINITIALIZED;
static inline xTracebackMode get_traceback_mode() {
    static std::atomic<xTracebackMode> g_traceback_mode(XTB_MODE_UNINITIALIZED);
    return g_traceback_mode;
}

// 获取当前模式
static inline xTracebackMode xtraceback_get_mode() {
    return g_traceback_mode.load(std::memory_order_acquire);
}

// 设置模式
static inline void xtraceback_set_mode(xTracebackMode mode) {
    g_traceback_mode.store(mode, std::memory_order_release);
}

#if XCORO_PLATFORM_UNIX

// 检测是否有调试工具可用
static inline bool xtraceback_has_debug_tools() {
#if XCORO_PLATFORM_LINUX
    // Linux: 检查 addr2line
    return system("which addr2line > /dev/null 2>&1") == 0;
#elif XCORO_PLATFORM_APPLE
    // macOS: 检查 atos
    return system("which atos > /dev/null 2>&1") == 0;
#else
    return false;
#endif
}

// 检测当前程序是否包含调试信息
static inline bool xtraceback_has_debug_symbols() {
    // 简单检测：通过 dladdr 是否能解析当前函数的符号
    Dl_info info;
    if (dladdr((void*)xtraceback_has_debug_symbols, &info)) {
        return info.dli_sname != nullptr;
    }
    return false;
}

// 检测是否是用户环境
static inline bool xtraceback_is_user_environment() {
    // 用户环境 = 没有调试工具 OR 没有调试符号
    return !(xtraceback_has_debug_tools() && xtraceback_has_debug_symbols());
}

#else // Windows
// Windows版本：总是假设有调试支持（因为有dbghelp）
static inline bool xtraceback_is_user_environment() {
    return false;
}
#endif

// ==============================================
// 初始化函数（启动时调用）
// ==============================================

// 强制使用简单模式
static inline void xtraceback_force_simple() {
    xlog_info("[xtraceback] Forcing simple stack trace mode");
    xtraceback_set_mode(XTB_MODE_SIMPLE);
}

// 强制使用详细模式
static inline void xtraceback_force_detailed() {
    xlog_info("[xtraceback] Forcing detailed stack trace mode");
    xtraceback_set_mode(XTB_MODE_DETAILED);
}

// 自动检测模式
static inline void xtraceback_auto_detect() {
    if (xtraceback_is_user_environment()) {
        xlog_info("[xtraceback] User environment detected, using simple mode");
        xtraceback_set_mode(XTB_MODE_SIMPLE);
    }
    else {
        xlog_info("[xtraceback] Development environment detected, using detailed mode");
        xtraceback_set_mode(XTB_MODE_DETAILED);
    }
}

// 初始化堆栈跟踪系统
static inline bool xtraceback_init() {
    // 检查是否已经初始化
    if (xtraceback_get_mode() != XTB_MODE_UNINITIALIZED) {
        return true;
    }

    // 检查环境变量
    const char* env_simple = getenv("XTRACEBACK_SIMPLE");
    const char* env_detailed = getenv("XTRACEBACK_DETAILED");

    if (env_simple && env_simple[0] == '1') {
        xtraceback_force_simple();
    } else if (env_detailed && env_detailed[0] == '1') {
        xtraceback_force_detailed();
    } else {
        xtraceback_auto_detect();
    }

    // 记录最终模式
    xlog_info("[xtraceback] Initialized with mode: %d", xtraceback_get_mode());
    return true;
}

// ==============================================
// 内部辅助函数
// ==============================================

#if XCORO_PLATFORM_UNIX
// 获取模块基地址
static inline uintptr_t get_module_base_address() {
    Dl_info info;
    if (dladdr((void*)get_module_base_address, &info)) {
        return (uintptr_t)info.dli_fbase;
    }
    return 0;
}

// 解析地址信息
static inline void print_address_info(uintptr_t addr, const char* prefix) {
    Dl_info info;
    if (dladdr((void*)addr, &info)) {
        uintptr_t offset = addr - (uintptr_t)info.dli_fbase;
        xlog_err("%s: 0x%lx (module: %s, offset: 0x%lx)",
            prefix, addr, info.dli_fname ? info.dli_fname : "unknown", offset);
    } else {
        xlog_err("%s: 0x%lx (unknown module)", prefix, addr);
    }
}
#endif

// ==============================================
// 平台特定的堆栈跟踪实现
// ==============================================

#if XCORO_PLATFORM_WINDOWS
// Windows堆栈跟踪
static inline void windows_stack_trace(PEXCEPTION_POINTERS ExceptionInfo = nullptr) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    
    // 初始化符号处理
    if (!SymInitialize(process, NULL, TRUE)) {
        xlog_err("SymInitialize failed, error: %lu", GetLastError());
        return;
    }
    
    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
    
    // 获取上下文
    CONTEXT context;
    if (ExceptionInfo) {
        context = *ExceptionInfo->ContextRecord;
    } else {
        RtlCaptureContext(&context);
    }
    
    // 初始化堆栈帧
    STACKFRAME64 stackFrame;
    ZeroMemory(&stackFrame, sizeof(STACKFRAME64));
    
#ifdef _M_AMD64
    stackFrame.AddrPC.Offset = context.Rip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Rsp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Rsp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#else
    stackFrame.AddrPC.Offset = context.Eip;
    stackFrame.AddrPC.Mode = AddrModeFlat;
    stackFrame.AddrFrame.Offset = context.Ebp;
    stackFrame.AddrFrame.Mode = AddrModeFlat;
    stackFrame.AddrStack.Offset = context.Esp;
    stackFrame.AddrStack.Mode = AddrModeFlat;
#endif
    
    xlog_err("Stack trace:");
    int frameCount = 0;
    
    while (frameCount < 10) {
        if (!StackWalk64(
#ifdef _M_AMD64
            IMAGE_FILE_MACHINE_AMD64,
#else
            IMAGE_FILE_MACHINE_I386,
#endif
            process, thread, &stackFrame, &context, NULL,
            SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
            break;
        }
        
        if (stackFrame.AddrPC.Offset == 0) break;
        frameCount++;
        
        // 获取符号信息
        DWORD64 displacement = 0;
        char buffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(TCHAR)];
        PSYMBOL_INFO pSymbol = (PSYMBOL_INFO)buffer;
        pSymbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        pSymbol->MaxNameLen = MAX_SYM_NAME;
        
        if (SymFromAddr(process, stackFrame.AddrPC.Offset, &displacement, pSymbol)) {
            // 获取行号信息
            IMAGEHLP_LINE64 line;
            line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            DWORD lineDisplacement = 0;
            
            if (SymGetLineFromAddr64(process, stackFrame.AddrPC.Offset, &lineDisplacement, &line)) {
                xlog_err("  [%d] %s - %s:%d", frameCount, pSymbol->Name, line.FileName, line.LineNumber);
            } else {
                xlog_err("  [%d] %s + 0x%llx", frameCount, pSymbol->Name, displacement);
            }
        } else {
            xlog_err("  [%d] 0x%llx", frameCount, stackFrame.AddrPC.Offset);
        }
    }
    
    if (frameCount == 0) {
        xlog_err("  No stack frames captured");
    }
    
    SymCleanup(process);
}

#elif XCORO_PLATFORM_APPLE
static inline void print_macos_ucontext(ucontext_t* ucontext) {
    if (!ucontext) return;
    xlog_err("=== STACK TRACE FROM EXCEPTION CONTEXT (macOS) ===");

#ifdef __x86_64__
    // 从 ucontext 获取寄存器
    _STRUCT_MCONTEXT* mctx = ucontext->uc_mcontext;
    void* rip = (void*)mctx->__ss.__rip;
    void* rbp = (void*)mctx->__ss.__rbp;
    void* rsp = (void*)mctx->__ss.__rsp;

    xlog_err("Exception registers:");
    xlog_err("  RIP: 0x%016lx (instruction pointer)", (uintptr_t)rip);
    xlog_err("  RBP: 0x%016lx (frame pointer)", (uintptr_t)rbp);
    xlog_err("  RSP: 0x%016lx (stack pointer)", (uintptr_t)rsp);

    // 打印异常发生的位置
    Dl_info rip_info;
    if (dladdr(rip, &rip_info)) {
        const char* func_name = rip_info.dli_sname ? rip_info.dli_sname : "??";
        uintptr_t offset = (uintptr_t)rip - (uintptr_t)rip_info.dli_fbase;

        // Demangle C++ 符号
        int status;
        char* demangled = abi::__cxa_demangle(func_name, NULL, NULL, &status);
        if (status == 0 && demangled) {
            xlog_err("Exception at: %s + 0x%lx", demangled, offset);
            free(demangled);
        }
        else {
            xlog_err("Exception at: %s + 0x%lx", func_name, offset);
        }
    }
    else {
        xlog_err("Exception at: 0x%016lx", (uintptr_t)rip);
    }

    // 改进的帧指针遍历
    xlog_err("\nStack frames (following RBP chain):");

    void** frame_ptr = (void**)rbp;
    int frame_count = 0;
    const int max_frames = 50;

    while (frame_ptr && frame_count < max_frames) {
        // 确保帧指针在合理的栈范围内
        if ((uintptr_t)frame_ptr < (uintptr_t)rsp) {
            break;  // 帧指针不能小于栈指针
        }

        // 返回地址在 RBP+8 的位置
        void* return_addr = frame_ptr[1];

        if (!return_addr || return_addr == 0) {
            break;
        }

        // 检查返回地址是否在可执行内存范围内
        Dl_info info;
        if (dladdr(return_addr, &info)) {
            const char* func_name = info.dli_sname ? info.dli_sname : "??";

            // Demangle C++ 符号
            int status;
            char* demangled = abi::__cxa_demangle(func_name, NULL, NULL, &status);
            if (status == 0 && demangled) {
                xlog_err("  [%2d] 0x%016lx %s",
                    frame_count, (uintptr_t)return_addr, demangled);
                free(demangled);
            }
            else {
                xlog_err("  [%2d] 0x%016lx %s",
                    frame_count, (uintptr_t)return_addr, func_name);
            }
        }
        else {
            xlog_err("  [%2d] 0x%016lx", frame_count, (uintptr_t)return_addr);
        }

        // 移动到上一帧（当前 RBP 指向上一帧的 RBP）
        void** next_frame = (void**)*frame_ptr;
        if (!next_frame || next_frame == frame_ptr || next_frame <= frame_ptr) {
            break;  // 栈帧结束
        }
        frame_ptr = next_frame;
        frame_count++;
    }

    if (frame_count == 0) {
        xlog_err("  (No stack frames found via frame pointer)");
    }

    // 尝试使用栈扫描作为备选方案
    xlog_err("\nStack frames (scanning stack memory):");
    frame_count = 0;

    // 扫描栈内存，寻找可能的返回地址
    void** stack_bottom = (void**)rsp;
    void** stack_top = (void**)((uintptr_t)rsp + 4096);  // 扫描4KB栈内存

    for (void** ptr = stack_bottom; ptr < stack_top && frame_count < 20; ptr++) {
        void* possible_addr = *ptr;

        // 检查是否为有效的代码地址
        if (possible_addr && possible_addr != 0) {
            Dl_info info;
            if (dladdr(possible_addr, &info) && info.dli_sname) {
                // 检查地址是否在代码段
                if (info.dli_fname && strstr(info.dli_fname, "svr")) {
                    const char* func_name = info.dli_sname;

                    // Demangle C++ 符号
                    int status;
                    char* demangled = abi::__cxa_demangle(func_name, NULL, NULL, &status);
                    if (status == 0 && demangled) {
                        xlog_err("  [%2d] 0x%016lx %s (stack scan)",
                            frame_count, (uintptr_t)possible_addr, demangled);
                        free(demangled);
                        frame_count++;
                    }
                }
            }
        }
    }

    // 打印当前信号处理器栈以供对比
    xlog_err("\n=== CURRENT SIGNAL HANDLER STACK CONTEXT (macOS) ===");
#endif  // __x86_64__
}

// macOS堆栈跟踪
static inline void print_macos_stack_trace(void* ucontext = nullptr) {
    print_macos_ucontext((ucontext_t*)ucontext);

    void* callstack[256];
    int frames = backtrace(callstack, 256);

    // 获取可执行文件路径
    char exe_path[1024];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        strcpy(exe_path, "unknown");
    }

    xlog_err("=== macOS STACK TRACE (%d frames) ===", frames);
    xlog_err("Executable: %s", exe_path);

    // 使用 atos 获取符号信息
    for (int i = 0; i < frames && i < 20; ++i) {
        Dl_info info;
        if (dladdr(callstack[i], &info)) {
            uintptr_t offset = (uintptr_t)callstack[i] - (uintptr_t)info.dli_fbase;

            // 获取模块名
            const char* module_name = "unknown";
            if (info.dli_fname) {
                const char* slash = strrchr(info.dli_fname, '/');
                module_name = slash ? slash + 1 : info.dli_fname;
            }

            // 使用 atos 获取更详细的信息
            char cmd[512];
            // 注意：atos 需要加载地址（slide），这里我们使用模块基地址
            snprintf(cmd, sizeof(cmd),
                "xcrun atos -o '%s' -l 0x%lx 0x%lx 2>/dev/null",
                exe_path, (uintptr_t)info.dli_fbase, (uintptr_t)callstack[i]);

            FILE* pipe = popen(cmd, "r");
            if (pipe) {
                char buffer[256];
                xlog_err("  [%2d] 0x%016lx", i, (uintptr_t)callstack[i]);

                while (fgets(buffer, sizeof(buffer), pipe)) {
                    char* newline = strchr(buffer, '\n');
                    if (newline) *newline = '\0';
                    if (strlen(buffer) > 0) {
                        xlog_err("       %s", buffer);
                    }
                }
                pclose(pipe);
            }
            else {
                // 如果 atos 失败，使用 dladdr 的信息
                if (info.dli_sname) {
                    // 尝试 demangle C++ 符号
                    int status = 0;
                    char* demangled = abi::__cxa_demangle(info.dli_sname, NULL, NULL, &status);
                    if (status == 0 && demangled) {
                        xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                            i, (uintptr_t)callstack[i], demangled, offset, module_name);
                        free(demangled);
                    }
                    else {
                        xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                            i, (uintptr_t)callstack[i], info.dli_sname, offset, module_name);
                    }
                }
                else {
                    xlog_err("  [%2d] 0x%016lx ?? + 0x%lx [%s]",
                        i, (uintptr_t)callstack[i], offset, module_name);
                }
            }
        }
        else {
            xlog_err("  [%2d] 0x%016lx", i, (uintptr_t)callstack[i]);
        }
    }
}

static inline void print_macos_stack_trace_simple(void* ucontext = nullptr) {
    print_macos_ucontext((ucontext_t*)ucontext);

    void* callstack[256];
    int frames = backtrace(callstack, 256);

    xlog_err("macOS Stack trace (%d frames):", frames);
    // 获取可执行文件路径
    char exe_path[1024];
    uint32_t size = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &size) != 0) {
        strcpy(exe_path, "unknown");
    }

    for (int i = 0; i < frames && i < 20; ++i) {
        Dl_info info;
        if (dladdr(callstack[i], &info)) {
            uintptr_t offset = (uintptr_t)callstack[i] - (uintptr_t)info.dli_fbase;
            const char* func_name = info.dli_sname ? info.dli_sname : "??";
            const char* module_name = info.dli_fname ? info.dli_fname : "unknown";

            // 尝试 demangle C++ 符号
            int status;
            char* demangled = abi::__cxa_demangle(func_name, nullptr, nullptr, &status);
            if (status == 0 && demangled) {
                xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                    i, (uintptr_t)callstack[i], demangled, offset, module_name);
                free(demangled);
            }
            else {
                xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                    i, (uintptr_t)callstack[i], func_name, offset, module_name);
            }
        }
        else {
            xlog_err("  [%2d] 0x%p", i, callstack[i]);
        }
    }
}

#elif XCORO_PLATFORM_LINUX
// Linux堆栈跟踪
static inline void print_linux_stack_trace_simple(void* ucontext = nullptr) {
    void* callstack[256];
    int frames = backtrace(callstack, 256);

    xlog_err("Stack trace (%d frames):", frames);

    char** symbols = backtrace_symbols(callstack, frames);

    for (int i = 0; i < frames && i < 30; ++i) {
        if (symbols && symbols[i]) {
            xlog_err("  [%2d] %s", i, symbols[i]);
        } else {
            Dl_info info;
            if (dladdr(callstack[i], &info)) {
                uintptr_t offset = (uintptr_t)callstack[i] - (uintptr_t)info.dli_fbase;
                const char* func_name = info.dli_sname ? info.dli_sname : "??";
                const char* module_name = info.dli_fname ? info.dli_fname : "unknown";

                // 尝试 demangle C++ 符号
                int status;
                char* demangled = abi::__cxa_demangle(func_name, nullptr, nullptr, &status);
                if (status == 0 && demangled) {
                    xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                        i, (uintptr_t)callstack[i], demangled, offset, module_name);
                    free(demangled);
                } else {
                    xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                        i, (uintptr_t)callstack[i], func_name, offset, module_name);
                }
            }
            else {
                xlog_err("  [%2d] 0x%p", i, callstack[i]);
            }
        }
    }

    if (symbols) {
        free(symbols);
    }

    // 如果有上下文，打印寄存器信息
    if (ucontext) {
#ifdef __x86_64__
        ucontext_t* ctx = (ucontext_t*)ucontext;
        xlog_err("\nRegisters:");
        xlog_err("  RIP: 0x%016lx, RSP: 0x%016lx, RBP: 0x%016lx",
            (unsigned long)ctx->uc_mcontext.gregs[REG_RIP],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RSP],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RBP]);
#endif
    }
}

static inline void print_linux_stack_trace(void* ucontext = nullptr) {
    void* callstack[256];
    int frames = backtrace(callstack, 256);

    // 获取可执行文件路径
    char exe_path[1024];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
    }
    else {
        strcpy(exe_path, "unknown");
    }

    xlog_err("=== STACK TRACE (%d frames) ===", frames);
    xlog_err("Executable: %s", exe_path);

    // 使用 backtrace_symbols_fd 直接输出原始符号
    xlog_err("Raw symbols:");
    backtrace_symbols_fd(callstack, frames, STDERR_FILENO);

    xlog_err("\nParsed stack trace:");

    char** symbols = backtrace_symbols(callstack, frames);

    for (int i = 0; i < frames && i < 20; ++i) {
        Dl_info info;
        if (dladdr(callstack[i], &info)) {
            uintptr_t offset = (uintptr_t)callstack[i] - (uintptr_t)info.dli_fbase;

            // 尝试 demangle 函数名
            const char* func_name = "??";
            if (info.dli_sname) {
                int status = 0;
                char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
                if (status == 0 && demangled) {
                    func_name = demangled;
                    xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                        i, (uintptr_t)callstack[i], func_name, offset,
                        info.dli_fname ? info.dli_fname : "unknown");
                    free(demangled);
                }
                else {
                    func_name = info.dli_sname;
                    xlog_err("  [%2d] 0x%016lx %s + 0x%lx [%s]",
                        i, (uintptr_t)callstack[i], func_name, offset,
                        info.dli_fname ? info.dli_fname : "unknown");
                }
            }
            else {
                xlog_err("  [%2d] 0x%016lx ?? + 0x%lx [%s]",
                    i, (uintptr_t)callstack[i], offset,
                    info.dli_fname ? info.dli_fname : "unknown");
            }

            // 尝试使用 addr2line 获取文件行号（需要调试信息）
            char addr2line_cmd[512];
            snprintf(addr2line_cmd, sizeof(addr2line_cmd),
                "addr2line -e %s -f -p -C 0x%lx 2>/dev/null || echo '  (no debug info)'",
                exe_path, (uintptr_t)callstack[i]);

            FILE* pipe = popen(addr2line_cmd, "r");
            if (pipe) {
                char buffer[256];
                if (fgets(buffer, sizeof(buffer), pipe)) {
                    // 移除换行符
                    char* newline = strchr(buffer, '\n');
                    if (newline) *newline = '\0';
                    if (strlen(buffer) > 0) {
                        xlog_err("       %s", buffer);
                    }
                }
                pclose(pipe);
            }
        }
        else {
            xlog_err("  [%2d] 0x%016lx", i, (uintptr_t)callstack[i]);
        }
    }

    if (symbols) {
        free(symbols);
    }

    // 打印寄存器信息（如果提供了上下文）
    if (ucontext) {
#ifdef __x86_64__
        ucontext_t* ctx = (ucontext_t*)ucontext;
        xlog_err("\nRegisters:");
        xlog_err("  RIP: 0x%016lx, RSP: 0x%016lx, RBP: 0x%016lx",
            (unsigned long)ctx->uc_mcontext.gregs[REG_RIP],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RSP],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RBP]);
        xlog_err("  RAX: 0x%016lx, RBX: 0x%016lx, RCX: 0x%016lx",
            (unsigned long)ctx->uc_mcontext.gregs[REG_RAX],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RBX],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RCX]);
        xlog_err("  RDX: 0x%016lx, RSI: 0x%016lx, RDI: 0x%016lx",
            (unsigned long)ctx->uc_mcontext.gregs[REG_RDX],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RSI],
            (unsigned long)ctx->uc_mcontext.gregs[REG_RDI]);
#endif
    }
}
#endif // 平台特定实现结束

// ==============================================
// 统一的公共接口
// ==============================================

// 打印当前堆栈跟踪
static inline void xtraceback_print() {
    // 确保已初始化
    if (xtraceback_get_mode() == XTB_MODE_UNINITIALIZED) {
        xtraceback_init();
    }

    switch (xtraceback_get_mode()) {
    case XTB_MODE_SIMPLE:
#if XCORO_PLATFORM_WINDOWS
        windows_stack_trace();
#elif XCORO_PLATFORM_APPLE
        print_macos_stack_trace_simple();
#elif XCORO_PLATFORM_LINUX
        print_linux_stack_trace_simple();
#endif
        break;

    case XTB_MODE_DETAILED:
#if XCORO_PLATFORM_WINDOWS
        windows_stack_trace();
#elif XCORO_PLATFORM_APPLE
        print_macos_stack_trace();  // 已有的详细版本
#elif XCORO_PLATFORM_LINUX
        print_linux_stack_trace();
#endif
        break;

    default:
        // 默认使用简单模式
        xtraceback_set_mode(XTB_MODE_SIMPLE);
        xtraceback_print();
        break;
    }
}

// 打印带上下文的堆栈跟踪
static inline void xtraceback_with_ctx(void* context) {
    // 确保已初始化
    if (xtraceback_get_mode() == XTB_MODE_UNINITIALIZED) {
        xtraceback_init();
    }

    switch (xtraceback_get_mode()) {
    case XTB_MODE_SIMPLE:
#if XCORO_PLATFORM_WINDOWS
        windows_stack_trace((PEXCEPTION_POINTERS)context);
#elif XCORO_PLATFORM_APPLE
        print_macos_stack_trace_simple(context);
#elif XCORO_PLATFORM_LINUX
        print_linux_stack_trace_simple(context);
#endif
        break;

    case XTB_MODE_DETAILED:
#if XCORO_PLATFORM_WINDOWS
        windows_stack_trace((PEXCEPTION_POINTERS)context);
#elif XCORO_PLATFORM_APPLE
        print_macos_stack_trace(context);
#elif XCORO_PLATFORM_LINUX
        print_linux_stack_trace(context);
#endif
        break;

    default:
        // 默认使用简单模式
        xtraceback_set_mode(XTB_MODE_SIMPLE);
        xtraceback_with_ctx(context);
        break;
    }
}

// 获取地址信息（Unix平台专用）
#if XCORO_PLATFORM_UNIX
static inline void xtraceback_print_addr_ex(uintptr_t addr, const char* prefix) {
    print_address_info(addr, prefix);
}
#else
static inline void xtraceback_print_addr_ex(uintptr_t addr, const char* prefix) {
    xlog_err("%s: 0x%lx", prefix, addr);
}
#endif

// 获取信号名称
static inline const char* xtraceback_sig_name(int sig) {
    switch (sig) {
#if XCORO_PLATFORM_UNIX
        case SIGSEGV: return "SIGSEGV";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        case SIGABRT: return "SIGABRT";
        case SIGTRAP: return "SIGTRAP";
#endif
#if XCORO_PLATFORM_WINDOWS
        case (int)0xC0000005: return "EXCEPTION_ACCESS_VIOLATION";
        case (int)0xC0000094: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case (int)0xC00000FD: return "EXCEPTION_STACK_OVERFLOW";
        case (int)0xC0000374: return "STATUS_HEAP_CORRUPTION";
        case (int)0xC0000409: return "STATUS_STACK_BUFFER_OVERRUN";
        case (int)0xE06D7363: return "CPP_EH_EXCEPTION";
        case (int)0xC000008E: return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case (int)0xC0000090: return "EXCEPTION_FLT_INVALID_OPERATION";
        case (int)0xC0000091: return "EXCEPTION_FLT_OVERFLOW";
        case (int)0xC0000092: return "EXCEPTION_FLT_UNDERFLOW";
        case (int)0xC0000093: return "EXCEPTION_FLT_INEXACT_RESULT";
#endif
        default: return "Unknown";
    }
}

// 获取信号描述
static inline const char* xtraceback_get_sig_desc(int sig, int si_code) {
    (void)sig; (void)si_code;
#if XCORO_PLATFORM_UNIX
    if (sig == SIGSEGV) {
#ifdef SEGV_MAPERR
        if (si_code == SEGV_MAPERR) return "address not mapped";
        else if (si_code == SEGV_ACCERR) return "invalid permissions";
#endif
    } else if (sig == SIGFPE) {
#ifdef FPE_INTDIV
        switch (si_code) {
            case FPE_INTDIV: return "integer divide by zero";
            case FPE_INTOVF: return "integer overflow";
            case FPE_FLTDIV: return "floating point divide by zero";
            case FPE_FLTOVF: return "floating point overflow";
            case FPE_FLTUND: return "floating point underflow";
            case FPE_FLTRES: return "floating point inexact result";
            case FPE_FLTINV: return "invalid floating point operation";
            case FPE_FLTSUB: return "subscript out of range";
            default: return "unknown FPE code";
        }
#endif
    } else if (sig == SIGILL) {
#ifdef ILL_ILLOPC
        switch (si_code) {
            case ILL_ILLOPC: return "illegal opcode";
            case ILL_ILLOPN: return "illegal operand";
            case ILL_ILLADR: return "illegal addressing mode";
            case ILL_ILLTRP: return "illegal trap";
            case ILL_PRVOPC: return "privileged opcode";
            case ILL_PRVREG: return "privileged register";
            case ILL_COPROC: return "coprocessor error";
            case ILL_BADSTK: return "internal stack error";
            default: return "unknown ILL code";
        }
#endif
    }
#endif
    
    return "unknown";
}

#endif // XTRACEBACK_INL