// xcoroutine.cpp - Safe coroutine implementation with hardware exception protection

#include "xcoroutine.h"
#include <coroutine>
#include <unordered_map>
#include <memory>
#include <iostream>
#include <atomic>

#include "xmutex.h"
#include "xlog.h"

// Forward declaration
class xCoroService;

// Singleton instance
static xCoroService* _co_svs = nullptr;

// Thread local storage
static thread_local int _co_cid = -1;

// Global longjmp context for hardware exception protection (similar to sigLJ in daemon.c)
static thread_local xCoroutineLJ* g_current_lj = nullptr;

// Signal handler for Unix/Linux systems
#ifndef _WIN32
static void coroutine_signal_handler(int sig, siginfo_t* info, void* context) {
    if (g_current_lj && g_current_lj->in_protected_call) {
        g_current_lj->sig = sig;

        // 记录详细的信号信息
        xlog_err("Hardware signal %d caught in coroutine at address %p",
                 sig, info->si_addr);

        // 获取调用栈（可选，用于调试）
        void* callstack[128];
        int frames = backtrace(callstack, 128);
        char** strs = backtrace_symbols(callstack, frames);
        if (strs) {
            xlog_err("Backtrace (%d frames):", frames);
            for (int i = 0; i < frames && i < 10; ++i) {  // 只显示前10帧
                xlog_err("  %s", strs[i]);
            }
            free(strs);
        }

        // 跳转回保护点
        siglongjmp(g_current_lj->buf, 1);
    } else {
        // 非保护调用中的信号，恢复默认处理
        xlog_err("Signal %d in non-protected context, terminating", sig);
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

// 安装信号处理器
static void install_signal_handlers() {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;  // 使用SA_SIGINFO获取更多信息
    sa.sa_sigaction = coroutine_signal_handler;  // 使用sa_sigaction而不是sa_handler

    // 安装信号处理器
    sigaction(SIGSEGV, &sa, nullptr);  // 段错误
    sigaction(SIGFPE, &sa, nullptr);   // 浮点异常/除零
    sigaction(SIGILL, &sa, nullptr);   // 非法指令
    sigaction(SIGBUS, &sa, nullptr);   // 总线错误
    sigaction(SIGTRAP, &sa, nullptr);  // 断点/跟踪陷阱
    sigaction(SIGABRT, &sa, nullptr);  // 中止信号

    xlog_info("Linux signal handlers installed");
}
#else
// Windows: Use structured exception handling
static LONG WINAPI global_exception_filter(PEXCEPTION_POINTERS ExceptionInfo) {
    xlog_err("*** GLOBAL EXCEPTION FILTER: Exception code: 0x%08X ***",
             ExceptionInfo->ExceptionRecord->ExceptionCode);

    // 如果是访问违例等严重错误，尝试记录并退出
    switch (ExceptionInfo->ExceptionRecord->ExceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION:
            xlog_err("Access violation occurred");
            break;
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            xlog_err("Integer divide by zero");
            break;
        case EXCEPTION_STACK_OVERFLOW:
            xlog_err("Stack overflow");
            break;
        default:
            xlog_err("Unknown exception");
            break;
    }

    // 返回 EXCEPTION_EXECUTE_HANDLER 会让程序继续执行到最近的 __except 块
    // 返回 EXCEPTION_CONTINUE_SEARCH 会让系统继续寻找异常处理器
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI coroutine_exception_handler(PEXCEPTION_POINTERS ExceptionInfo) {
    if (g_current_lj && g_current_lj->in_protected_call) {
        g_current_lj->sig = ExceptionInfo->ExceptionRecord->ExceptionCode;
        xlog_err("Hardware exception caught in coroutine: code 0x%08X",
                 ExceptionInfo->ExceptionRecord->ExceptionCode);
        longjmp(g_current_lj->buf, 1);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_signal_handlers() {
    // Install vectored exception handler for Windows
    SetUnhandledExceptionFilter(global_exception_filter);
    AddVectoredExceptionHandler(1, coroutine_exception_handler);
}
#endif

// Mutex RAII wrapper
class XMutexGuard {
public:
    explicit XMutexGuard(xMutex* mutex) : m_mutex(mutex) {
        if (m_mutex) xnet_mutex_lock(m_mutex);
    }
    ~XMutexGuard() {
        if (m_mutex) xnet_mutex_unlock(m_mutex);
    }
    XMutexGuard(const XMutexGuard&) = delete;
    XMutexGuard& operator=(const XMutexGuard&) = delete;
private:
    xMutex* m_mutex;
};

// Safe resume implementation for xTask
bool xTask::resume_safe(void* param, xCoroutineLJ* lj) {
if (!handle_ || handle_.done()) return false;

   // Save current LJ state
   xCoroutineLJ* old_lj = g_current_lj;
   g_current_lj = lj;
   lj->in_protected_call = true;
   lj->sig = 0;

   bool success = false;

   // 硬件异常保护
#ifdef _WIN32
   if (setjmp(lj->buf) == 0) {
#else
   if (sigsetjmp(lj->buf, 1) == 0) {  // Linux下使用sigsetjmp，第二个参数1表示保存信号掩码
#endif
       // 正常执行路径
       try {
           handle_.resume();
           success = true;
       } catch (const std::exception& e) {
           xlog_err("C++ exception in coroutine %d: %s",
                    handle_.promise().coroutine_id, e.what());
           if (!handle_.promise().exception_ptr) {
               handle_.promise().exception_ptr = std::current_exception();
           }
           success = false;
       } catch (...) {
           xlog_err("Unknown C++ exception in coroutine %d", handle_.promise().coroutine_id);
           if (!handle_.promise().exception_ptr) {
               handle_.promise().exception_ptr = std::current_exception();
           }
           success = false;
       }
   } else {
       // 硬件异常路径
       xlog_err("*** HARDWARE EXCEPTION CAUGHT in coroutine %d: signal %d ***",
                handle_.promise().coroutine_id, lj->sig);
       handle_.promise().hardware_signal = lj->sig;
       success = false;
   }

   lj->in_protected_call = false;
   g_current_lj = old_lj;
   return success;
}

// Coroutine manager implementation
class xCoroService {
public:
    // Forward declaration of xCoro
    struct xCoro;

    std::unordered_map<int, std::unique_ptr<xCoro>> coroutine_map_;
    mutable xMutex map_mutex;
    mutable xMutex wait_mutex;
    std::atomic<int> next_coroutine_id_{ 0 };
    std::atomic<uint32_t> next_wait_id_{ 0 };

    struct PendingWait {
        std::coroutine_handle<> handle = nullptr;
        std::unique_ptr<std::vector<VariantType>> result;
        bool done = false;
        int coro_id = -1;
    };
    std::unordered_map<uint32_t, PendingWait> wait_map_;

    int generate_coroutine_id() { return ++next_coroutine_id_; }
    uint32_t generate_wait_id() { return ++next_wait_id_; }

public:
    xCoroService() {
        xnet_mutex_init(&map_mutex);
        xnet_mutex_init(&wait_mutex);

        // Install hardware exception handlers
    }

    ~xCoroService() {
        {
            XMutexGuard lock(&map_mutex);
            coroutine_map_.clear();
        }
        {
            XMutexGuard lock(&wait_mutex);
            wait_map_.clear();
        }
        xnet_mutex_uninit(&map_mutex);
        xnet_mutex_uninit(&wait_mutex);
    }

    // xCoro definition inside xCoroService
    struct xCoro {
        xTask task;
        int coroutine_id;
        xCoroutineLJ lj;  // Hardware exception protection context

        xCoro(xTask&& t, int id) : task(std::move(t)), coroutine_id(id) {
            task.get_promise().coroutine_id = id;
            lj.env = nullptr;
            lj.sig = 0;
            lj.in_protected_call = false;
        }
        ~xCoro() = default;
        xCoro(const xCoro&) = delete;
        xCoro& operator=(const xCoro&) = delete;

        bool is_done() const {
            return task.done();
        }

        bool resume_safe(void* param) {
            if (is_done()) return false;

            _co_cid = coroutine_id;
            bool success = task.resume_safe(param, &lj);
            _co_cid = -1;

            // 检查并处理任何类型的异常
            if (task.has_any_exception()) {
                std::string error_msg = task.get_exception_message();
                xlog_err("Coroutine %d has exception: %s", coroutine_id, error_msg.c_str());

                // 标记协程为完成状态
                if (_co_svs) {
                    _co_svs->remove_coroutine(coroutine_id);
                }
                return false;
            }

            return success;
        }

        int get_id() const { return coroutine_id; }

        // Check if coroutine ended due to hardware exception
        bool has_hardware_exception() const {
            return task.get_promise().has_hardware_exception();
        }

        // Get hardware exception message
        std::string get_hardware_exception_message() const {
            return task.get_promise().get_hardware_exception_message();
        }
    };

    xCoro* find_coroutine_by_id(int id) {
        XMutexGuard lock(&map_mutex);
        auto it = coroutine_map_.find(id);
        return (it != coroutine_map_.end()) ? it->second.get() : nullptr;
    }

    int run_task(fnCoro func, void* arg) {
        int coro_id = generate_coroutine_id();
        int old_id = _co_cid;
        _co_cid = coro_id;

        xTask task;
        xCoroutineLJ creation_lj;
        creation_lj.env = nullptr;
        creation_lj.sig = 0;
        creation_lj.in_protected_call = true;

        // Use hardware exception protection for coroutine creation
        xCoroutineLJ* old_lj = g_current_lj;
        g_current_lj = &creation_lj;

        // Use setjmp/longjmp for both Windows and Unix
        if (setjmp(creation_lj.buf) == 0) {
            task = func(arg);
        } else {
            // Hardware exception during coroutine creation
            xlog_err("Coroutine %d creation crashed with hardware exception", coro_id);
            task = xTask{};
        }

        creation_lj.in_protected_call = false;
        g_current_lj = old_lj;
        _co_cid = old_id;

        if (task.done() || !task.handle_) {
            return coro_id;
        }

        task.get_promise().coroutine_id = coro_id;

        XMutexGuard guard(&map_mutex);
        coroutine_map_[coro_id] = std::make_unique<xCoro>(std::move(task), coro_id);
        return coro_id;
    }

    bool resume_coroutine(int id, void* param) {
        xCoro* coro = find_coroutine_by_id(id);
        if (!coro) return false;

        if (!coro->is_done()) {
            bool success = coro->resume_safe(param);
            if (coro->is_done() || !success) {
                remove_coroutine(id);
            }
            return success;
        }
        remove_coroutine(id);
        return false;
    }

    bool remove_coroutine(int id) {
        XMutexGuard lock(&map_mutex);
        auto it = coroutine_map_.find(id);
        if (it != coroutine_map_.end()) {
            coroutine_map_.erase(it);
            return true;
        }
        return false;
    }

    bool is_coroutine_done(int id) {
        xCoro* coro = find_coroutine_by_id(id);
        return coro ? coro->is_done() : true;
    }

    void resume_all() {
        std::vector<int> ids;
        {
            XMutexGuard lock(&map_mutex);
            for (auto& [id, coro] : coroutine_map_) {
                if (!coro->is_done()) ids.push_back(id);
            }
        }
        for (int id : ids) {
            xCoro* coro = find_coroutine_by_id(id);
            if (coro && !coro->is_done()) coro->resume_safe(nullptr);
        }
    }

    size_t get_active_count() const {
        XMutexGuard lock(&map_mutex);
        size_t count = 0;
        for (const auto& [id, coro] : coroutine_map_) {
            if (!coro->is_done()) {
                count++;
            }
        }
        return count;
    }
private:
    void resume_with_hw_protection(std::coroutine_handle<> handle, int coro_id, const char* context) {
    if (!handle || handle.done()) return;

        // 创建临时的硬件保护上下文
        xCoroutineLJ temp_lj;
        temp_lj.env = nullptr;
        temp_lj.sig = 0;
        temp_lj.in_protected_call = true;

        xCoroutineLJ* old_lj = g_current_lj;
        g_current_lj = &temp_lj;
        int old_cid = _co_cid;
        _co_cid = coro_id;

        xlog_info("Safe resuming coroutine %d with HW protection in context: %s", coro_id, context);

        // 硬件异常保护
#ifdef _WIN32
        if (setjmp(temp_lj.buf) == 0) {
#else
        if (sigsetjmp(temp_lj.buf, 1) == 0) {  // Linux下使用sigsetjmp
#endif
            try {
                handle.resume();
                xlog_info("Coroutine %d resumed successfully", coro_id);
            } catch (const std::exception& e) {
                xlog_err("C++ exception in coroutine %d: %s", coro_id, e.what());
            } catch (...) {
                xlog_err("Unknown exception in coroutine %d", coro_id);
            }
        } else {
            // 硬件异常被捕获
            xlog_err("*** HW EXCEPTION in coroutine %d during %s: signal %d ***",
                    coro_id, context, temp_lj.sig);

               // 记录Linux特定的信号信息
   #ifndef _WIN32
               switch (temp_lj.sig) {
                   case SIGSEGV:
                       xlog_err("Segmentation fault in coroutine %d", coro_id);
                       break;
                   case SIGFPE:
                       xlog_err("Floating point exception in coroutine %d", coro_id);
                       break;
                   case SIGBUS:
                       xlog_err("Bus error in coroutine %d", coro_id);
                       break;
                   case SIGILL:
                       xlog_err("Illegal instruction in coroutine %d", coro_id);
                       break;
                   default:
                       xlog_err("Signal %d in coroutine %d", temp_lj.sig, coro_id);
                       break;
               }
   #endif
           }

           temp_lj.in_protected_call = false;
           g_current_lj = old_lj;
           _co_cid = old_cid;
       }
public:
    // -------------------- Wait related --------------------
    void register_waiter(uint32_t wait_id, std::coroutine_handle<> h, int coro_id) {
        std::coroutine_handle<> to_resume = nullptr;
        int resume_coro_id = -1;
        {
            XMutexGuard lock(&wait_mutex);
            auto& p = wait_map_[wait_id];
            p.handle = h;
            p.coro_id = coro_id;
            if (p.done && p.result) {
                to_resume = p.handle;
                resume_coro_id = p.coro_id;
            }
        }
        if (to_resume) {
            resume_with_hw_protection(to_resume, resume_coro_id, "register_waiter");
        }
    }

    void resume_waiter(uint32_t wait_id, std::vector<VariantType>&& resp) {
        std::coroutine_handle<> to_resume = nullptr;
        int resume_coro_id = -1;
        {
            XMutexGuard lock(&wait_mutex);
            auto& p = wait_map_[wait_id];
            p.result = std::make_unique<std::vector<VariantType>>(std::move(resp));
            p.done = true;
            if (p.handle) {
                to_resume = p.handle;
                resume_coro_id = p.coro_id;
            }
        }
        if (to_resume) {
            resume_with_hw_protection(to_resume, resume_coro_id, "resume_waiter");
        }
    }

    std::vector<VariantType> take_wait_result(uint32_t wait_id) {
        XMutexGuard lock(&wait_mutex);
        auto it = wait_map_.find(wait_id);
        if (it == wait_map_.end() || !it->second.result) return {};
        auto r = std::move(*it->second.result);
        wait_map_.erase(it);
        return r;
    }
};

// -------------------- Awaiter implementation --------------------
std::coroutine_handle<> xFinAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    if (_co_svs && coroutine_id > 0) {
        _co_svs->remove_coroutine(coroutine_id);
    }
    return std::noop_coroutine();
}

xAwaiter::xAwaiter() noexcept
    : wait_id_(_co_svs ? _co_svs->generate_wait_id() : 0)
    , error_code_(0)
    , coro_id_(_co_cid) {  // Save current coroutine ID
}

xAwaiter::xAwaiter(int err) noexcept
    : wait_id_(0)
    , error_code_(err)
    , coro_id_(-1) {
}

void xAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    if (!_co_svs) return;
    _co_svs->register_waiter(wait_id_, h, coro_id_);
}

std::vector<VariantType> xAwaiter::await_resume() noexcept {
    if (error_code_ != 0) {
        std::vector<VariantType> err;
        err.emplace_back(error_code_);
        return err;
    }
    if (!_co_svs) return {};

    // 添加异常保护
    try {
        return _co_svs->take_wait_result(wait_id_);
    } catch (const std::bad_variant_access& e) {
        xlog_err("Variant access exception in await_resume for coroutine %d: %s", coro_id_, e.what());
        // 返回错误结果
        std::vector<VariantType> err;
        err.emplace_back(-1);  // 错误码
        err.emplace_back((std::string("Variant access error: ") + e.what()).c_str());
        return err;
    } catch (const std::exception& e) {
        xlog_err("Exception in await_resume for coroutine %d: %s", coro_id_, e.what());
        std::vector<VariantType> err;
        err.emplace_back(-1);
        err.emplace_back((std::string("Exception: ") + e.what()).c_str());
        return err;
    } catch (...) {
        xlog_err("Unknown exception in await_resume for coroutine %d", coro_id_);
        std::vector<VariantType> err;
        err.emplace_back(-1);
        err.emplace_back("Unknown exception");
        return err;
    }
}

// -------------------- External interfaces --------------------
bool coroutine_init() {
    if (_co_svs) return true;
    try {
        _co_svs = new xCoroService();
        install_signal_handlers();
        xlog_info("Coroutine system initialized with hardware exception protection");
        return true;
    } catch (...) {
        return false;
    }
}

void coroutine_uninit() {
    delete _co_svs;
    _co_svs = nullptr;
}

int coroutine_run(fnCoro func, void* arg) {
    return _co_svs ? _co_svs->run_task(func, arg) : -1;
}

bool coroutine_resume(int id, void* param) {
    return _co_svs ? _co_svs->resume_coroutine(id, param) : false;
}

void coroutine_resume_all() {
    if (_co_svs) _co_svs->resume_all();
}

bool coroutine_is_done(int id) {
    return _co_svs ? _co_svs->is_coroutine_done(id) : true;
}

size_t coroutine_get_active_count() {
    return _co_svs ? _co_svs->get_active_count() : 0;
}

int coroutine_self_id() {
    return _co_cid;
}

bool coroutine_resume(uint32_t wait_id, std::vector<VariantType>&& resp) {
    if (!_co_svs) return false;
    _co_svs->resume_waiter(wait_id, std::move(resp));
    return true;
}
