// xcoroutine.cpp - Safe coroutine implementation with hardware exception protection

#include "xcoroutine.h"
#include <unordered_map>
#include <memory>
#include <iostream>
#include <atomic>

#include "xmutex.h"
#include "xlog.h"

// 包含堆栈跟踪实现
#include "xtraceback.inl"
#include "xtimer.h"

// Forward declaration
class xCoroService;

// Singleton instance
static thread_local xCoroService* _co_svs = nullptr;

// Thread local storage
static thread_local int _co_cid = -1;

// Global longjmp context for hardware exception protection
static thread_local xCoroutineLJ* _cur_lj = nullptr;

// ==============================================
// 平台特定异常处理实现
// ==============================================

#if XCORO_PLATFORM_UNIX
// Unix信号处理器
static void coroutine_signal_handler(int sig, siginfo_t* info, void* context) {
    // 协程保护上下文中的硬件异常
    if (_cur_lj && _cur_lj->in_protected_call) {
        _cur_lj->sig = sig;
        
        // 记录详细的信号和地址信息
        xlog_err("=== HARDWARE EXCEPTION DETECTED ===");

        // 使用堆栈跟踪接口获取信号名称
        xlog_err("Signal: %d (%s)", sig, xtraceback_sig_name(sig));

        // 打印异常地址信息
        if (info && info->si_addr) {
            xtraceback_print_addr_ex((uintptr_t)info->si_addr, "Fault address");
        }

        // 提供更多关于信号的信息
        if (info) {
            xlog_err("Signal code: %d", info->si_code);

            // 使用堆栈跟踪接口获取信号描述
            const char* code_desc = xtraceback_get_sig_desc(sig, info->si_code);
            xlog_err("Signal code description: %s", code_desc);
        }

        // 使用统一的堆栈跟踪接口
        xtraceback_with_ctx(context);

        xlog_err("=== END EXCEPTION REPORT ===");

        // 跳转回协程保护点，协程退出但进程继续
        siglongjmp(_cur_lj->buf, 1);
    } else { // 非协程上下文的硬件异常（主线程或其他非协程代码）
        xlog_err("Signal %d in non-protected context, terminating process...", sig);

        // 打印堆栈跟踪
        xtraceback_with_ctx(context);

        // 非协程上下文中的异常是致命的，应该终止进程
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

// 安装Unix信号处理器
static void install_unix_signal_handlers() {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = coroutine_signal_handler;

#if XCORO_PLATFORM_APPLE
    sa.sa_flags |= SA_NODEFER;
#endif

    // 安装信号处理器
    sigaction(SIGSEGV, &sa, nullptr);  // 段错误
    sigaction(SIGFPE, &sa, nullptr);   // 浮点异常/除零
    sigaction(SIGILL, &sa, nullptr);   // 非法指令
    sigaction(SIGBUS, &sa, nullptr);   // 总线错误
    sigaction(SIGTRAP, &sa, nullptr);  // 断点/跟踪陷阱
    sigaction(SIGABRT, &sa, nullptr);  // 中止信号

    xlog_info("Unix signal handlers installed for %s",
#if XCORO_PLATFORM_APPLE
        "macOS"
#else
        "Linux"
#endif
    );
}

#elif XCORO_PLATFORM_WINDOWS
// Windows异常处理器
static LONG WINAPI coroutine_exception_handler(PEXCEPTION_POINTERS ExceptionInfo) {
    // 协程保护上下文中的异常
    if (_cur_lj && _cur_lj->in_protected_call) {
        _cur_lj->sig = ExceptionInfo->ExceptionRecord->ExceptionCode;

        xlog_err("=== WINDOWS HARDWARE EXCEPTION DETECTED ===");

        // 使用堆栈跟踪接口获取异常名称
        xlog_err("Exception: 0x%08X (%s)",
            ExceptionInfo->ExceptionRecord->ExceptionCode,
            xtraceback_sig_name(ExceptionInfo->ExceptionRecord->ExceptionCode));

        xlog_err("Exception address: 0x%p",
            ExceptionInfo->ExceptionRecord->ExceptionAddress);

        // 特殊处理：空指针函数调用
        if (ExceptionInfo->ExceptionRecord->ExceptionCode == 0xC0000005 &&
            ExceptionInfo->ExceptionRecord->ExceptionAddress == 0) {
            xlog_err("*** NULL POINTER FUNCTION CALL DETECTED ***");
            xlog_err("Attempted to call a function through a null pointer");
        } else {
            // 如果是访问违例，提供更多细节
            if (ExceptionInfo->ExceptionRecord->ExceptionCode == 0xC0000005) {
                // EXCEPTION_ACCESS_VIOLATION
                ULONG_PTR access_type = ExceptionInfo->ExceptionRecord->ExceptionInformation[0];
                ULONG_PTR violation_address = ExceptionInfo->ExceptionRecord->ExceptionInformation[1];
                const char* access_str = (access_type == 0) ? "read" :
                    (access_type == 1) ? "write" : "execute";
                xlog_err("Access violation: attempted to %s address 0x%p",
                    access_str, (void*)violation_address);
            }
        }

        // 使用统一的堆栈跟踪接口
        xtraceback_with_ctx(ExceptionInfo);

        // 添加协程上下文信息
        if (_cur_lj) {
            xlog_err("Coroutine context: protected_call=%d, signal=%d",
                _cur_lj->in_protected_call, _cur_lj->sig);
        }

        xlog_err("=== END EXCEPTION REPORT ===");

        longjmp(_cur_lj->buf, 1);
    }

    // 非协程上下文中的异常，继续搜索处理器
    return EXCEPTION_CONTINUE_SEARCH;
}

// Windows全局异常过滤器
static LONG WINAPI global_exception_filter(PEXCEPTION_POINTERS ExceptionInfo) {
    // 只在非协程环境中记录全局异常
    if (!_cur_lj || !_cur_lj->in_protected_call) {
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

        // 打印堆栈跟踪
        xtraceback_with_ctx(ExceptionInfo);

        xlog_err("Non-coroutine context exception is fatal, terminating process");
    }

    // 返回 EXCEPTION_CONTINUE_SEARCH 让系统继续寻找异常处理器
    return EXCEPTION_CONTINUE_SEARCH;
}

// 安装Windows异常处理器
static void install_windows_exception_handlers() {
    SetUnhandledExceptionFilter(global_exception_filter);
    AddVectoredExceptionHandler(1, coroutine_exception_handler);
    xlog_info("Windows exception handlers installed");
}
#endif // 平台判断结束

// ==============================================
// 统一的异常处理器安装
// ==============================================

static void install_signal_handlers() {
#if XCORO_PLATFORM_WINDOWS
    install_windows_exception_handlers();
#elif XCORO_PLATFORM_UNIX
    install_unix_signal_handlers();
#endif
}

// ==============================================
// Mutex RAII wrapper
// ==============================================

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

// ==============================================
// Safe resume implementation for xCoroTask
// ==============================================

bool xCoroTask::resume_safe(void* param, xCoroutineLJ* lj) {
    if (!handle_ || handle_.done()) return false;

    // Save current LJ state
    xCoroutineLJ* old_lj = _cur_lj;
    _cur_lj = lj;
    lj->in_protected_call = true;
    lj->sig = 0;

    bool success = false;

    // 硬件异常保护
#if XCORO_PLATFORM_WINDOWS
    if (setjmp(lj->buf) == 0) {
#else
    if (sigsetjmp(lj->buf, 1) == 0) {  // Linux下使用sigsetjmp，第二个参数1表示保存信号掩码
#endif
        // 正常执行路径
        try {
            handle_.resume();
            success = true;
        }
        catch (const std::exception& e) {
            xlog_err("C++ exception in coroutine %d: %s",
                handle_.promise().coroutine_id, e.what());
            if (!handle_.promise().exception_ptr) {
                handle_.promise().exception_ptr = std::current_exception();
            }
            success = false;
        }
        catch (...) {
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
    _cur_lj = old_lj;
    return success;
}

// ==============================================
// Safe resume implementation for xCoroTaskT<T>
// ==============================================

template<typename T>
bool xCoroTaskT<T>::resume_safe(void* param, xCoroutineLJ* lj) {
    if (!handle_ || handle_.done()) return false;

    // Save current LJ state
    xCoroutineLJ* old_lj = _cur_lj;
    _cur_lj = lj;
    lj->in_protected_call = true;
    lj->sig = 0;

    bool success = false;

    // 硬件异常保护
#if defined(_WIN32) || defined(_WIN64)
    if (setjmp(lj->buf) == 0) {
#else
    if (sigsetjmp(lj->buf, 1) == 0) {  // Linux下使用sigsetjmp，第二个参数1表示保存信号掩码
#endif
        // 正常执行路径
        try {
            handle_.resume();
            success = true;
        }
        catch (const std::exception& e) {
            xlog_err("C++ exception in coroutine %d: %s",
                handle_.promise().coroutine_id, e.what());
            if (!handle_.promise().exception_ptr) {
                handle_.promise().exception_ptr = std::current_exception();
            }
            success = false;
        }
        catch (...) {
            xlog_err("Unknown C++ exception in coroutine %d", handle_.promise().coroutine_id);
            if (!handle_.promise().exception_ptr) {
                handle_.promise().exception_ptr = std::current_exception();
            }
            success = false;
        }
    }
    else {
        // 硬件异常路径
        xlog_err("*** HARDWARE EXCEPTION CAUGHT in coroutine %d: signal %d ***",
            handle_.promise().coroutine_id, lj->sig);
        handle_.promise().hardware_signal = lj->sig;
        success = false;
    }

    lj->in_protected_call = false;
    _cur_lj = old_lj;
    return success;
}

// waiter timeout
void* coroutine_timer(uint32_t wait_id, int time_ms);

// ==============================================
// Coroutine manager implementation
// ==============================================

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
        std_coro::coroutine_handle<> handle = nullptr;
        std::unique_ptr<std::vector<VariantType>> result;
        bool done = false;
        int coro_id = -1;
        void* timer = nullptr;
    };
    std::unordered_map<uint32_t, PendingWait> wait_map_;

    int generate_coroutine_id() { return ++next_coroutine_id_; }
    uint32_t generate_wait_id() { return ++next_wait_id_; }

public:
    xCoroService() {
        xnet_mutex_init(&map_mutex);
        xnet_mutex_init(&wait_mutex);
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
        xCoroTask task;
        int coroutine_id;
        xCoroutineLJ lj;  // Hardware exception protection context

        xCoro(xCoroTask&& t, int id) : task(std::move(t)), coroutine_id(id) {
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

            int last = _co_cid;
            _co_cid = coroutine_id;
            bool success = task.resume_safe(param, &lj);
            _co_cid = last;
            
            if (_co_svs->coroutine_map_.find(coroutine_id) == _co_svs->coroutine_map_.end()) {
                xlog_debug("Coroutine %d has been removed from coroutine map", coroutine_id);
                return true;// 以及执行到尾部，则说明已经执行完毕且无异常
            }

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

        xCoroTask task;
        xCoroutineLJ creation_lj;
        creation_lj.env = nullptr;
        creation_lj.sig = 0;
        creation_lj.in_protected_call = true;

        // Use hardware exception protection for coroutine creation
        xCoroutineLJ* old_lj = _cur_lj;
        _cur_lj = &creation_lj;

        // Use setjmp/longjmp for both Windows and Unix
#if XCORO_PLATFORM_WINDOWS
        if (setjmp(creation_lj.buf) == 0) {
#else
        if (sigsetjmp(creation_lj.buf, 1) == 0) {
#endif
            task = func(arg);
        } else {
            // Hardware exception during coroutine creation
            xlog_err("=== COROUTINE CREATION EXCEPTION ===");
            xlog_err("*** HW EXCEPTION during coroutine %d creation: signal %d ***",
                coro_id, creation_lj.sig);

#if XCORO_PLATFORM_WINDOWS
            xlog_err("Windows exception code: 0x%08X", creation_lj.sig);
            // 打印简单堆栈跟踪
            xtraceback_print();
#else
            xlog_err("Unix signal: %d", creation_lj.sig);
#endif

            xlog_err("=== END CREATION EXCEPTION REPORT ===");
            task = xCoroTask{};
        }

        creation_lj.in_protected_call = false;
        _cur_lj = old_lj;
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
            coro = find_coroutine_by_id(id); // 重新获取-防止resume后被删除
            if (!coro) return success;
            if (coro->is_done() || !success) {
                // remove_coroutine(id);
            }
            return success;
        }
        // remove_coroutine(id);
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
    void resume_with_hw_protection(std_coro::coroutine_handle<> handle, int coro_id, const char* context) {
        if (!handle || handle.done()) return;

        // 创建临时的硬件保护上下文
        xCoroutineLJ temp_lj;
        temp_lj.env = nullptr;
        temp_lj.sig = 0;
        temp_lj.in_protected_call = true;

        xCoroutineLJ* old_lj = _cur_lj;
        _cur_lj = &temp_lj;
        int old_cid = _co_cid;
        _co_cid = coro_id;

        xlog_info("Safe resuming coroutine %d with HW protection in context: %s", coro_id, context);

#if XCORO_PLATFORM_WINDOWS
        if (setjmp(temp_lj.buf) == 0) {
#else
        if (sigsetjmp(temp_lj.buf, 1) == 0) {  // Linux下使用sigsetjmp
#endif
            try {
                handle.resume();
                xlog_info("Coroutine %d resumed successfully", coro_id);
            }
            catch (const std::exception& e) {
                xlog_err("C++ exception in coroutine %d: %s", coro_id, e.what());
            }
            catch (...) {
                xlog_err("Unknown exception in coroutine %d", coro_id);
            }
        } else {
            xlog_err("*** HW EXCEPTION in coroutine %d during %s: signal %d ***",
                coro_id, context, temp_lj.sig);

#if XCORO_PLATFORM_UNIX
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
        _cur_lj = old_lj;
        _co_cid = old_cid;
        }

public:
    // -------------------- Wait related --------------------
    void register_waiter(uint32_t wait_id, std_coro::coroutine_handle<> h, int coro_id, int timeout) {
        std_coro::coroutine_handle<> to_resume = nullptr;
        int resume_coro_id = -1;
        {
            XMutexGuard lock(&wait_mutex);
            auto& p = wait_map_[wait_id];
            p.handle = h;
            p.coro_id = coro_id;
            p.timer = nullptr;
            if (p.done && p.result) {
                to_resume = p.handle;
                resume_coro_id = p.coro_id;
            } else if(timeout > 0) {
                p.timer = coroutine_timer(wait_id, timeout);
            }
        }
        if (to_resume) {
            resume_with_hw_protection(to_resume, resume_coro_id, "register_waiter");
        }
    }

    void resume_waiter(uint32_t wait_id, std::vector<VariantType> && resp) {
        std_coro::coroutine_handle<> to_resume = nullptr;
        int resume_coro_id = -1;
        {
            XMutexGuard lock(&wait_mutex);
            auto it = wait_map_.find(wait_id);
            if (it == wait_map_.end())
                return;

            auto& p = it->second;
            p.result = std::make_unique<std::vector<VariantType>>(std::move(resp));
            p.done = true;
            if (p.handle) {
                to_resume = p.handle;
                resume_coro_id = p.coro_id;
            }

            if (p.timer) {
                xtimer_del((xtimerHandler)p.timer);
                p.timer = nullptr;
            }
        }
        if (to_resume) {
            resume_with_hw_protection(to_resume, resume_coro_id, "resume_waiter");
        }
    }

    void resume_waiter_timeout(uint32_t wait_id) {
        std_coro::coroutine_handle<> to_resume = nullptr;
        int resume_coro_id = -1;
        {
            XMutexGuard lock(&wait_mutex);
            auto& p = wait_map_[wait_id];

            char buf[32];
            snprintf(buf, sizeof(buf), "CoroWaiter %d timed out", wait_id);

            auto err = std::make_unique<std::vector<VariantType>>();
            err->push_back(VariantType(-1));
            err->push_back(XPackBuff(buf));

            p.result = std::move(err);
            p.done = true;
            if (p.handle) {
                to_resume = p.handle;
                resume_coro_id = p.coro_id;
            }
            if (p.timer) { /* delay timer auto deleted */
                p.timer = nullptr;
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

// ==============================================
// Awaiter implementation
// ==============================================

void xFinAwaiter::await_suspend(std_coro::coroutine_handle<> h) noexcept {
    if (_co_svs && coroutine_id > 0) {
        if (continuation) {
            // 子协程。恢复父协程。
            continuation.resume();
        } else {
            // 顶层协程。自我清理。
            _co_svs->remove_coroutine(coroutine_id);
        }
    } else if(continuation) {
        continuation.resume();
    }
}

xAwaiter::xAwaiter()
    : wait_id_(_co_svs ? _co_svs->generate_wait_id() : 0)
    , error_code_(0)
    , coro_id_(_co_cid)
    , timeout_(0){
}

xAwaiter::xAwaiter(int err)
    : wait_id_(0)
    , error_code_(err)
    , timeout_(0){
}

void xAwaiter::await_suspend(std_coro::coroutine_handle<> h) {
    if (!_co_svs) return;
    _co_svs->register_waiter(wait_id_, h, coro_id_, timeout_);
}

std::vector<VariantType> xAwaiter::await_resume() {
    if (error_code_ != 0) {
        std::vector<VariantType> err;
        err.emplace_back(error_code_);
        return err;
    }
    if (!_co_svs) return {};

    try {
        return _co_svs->take_wait_result(wait_id_);
    } catch (const std::bad_variant_access& e) {
        xlog_err("Variant access exception in await_resume for coroutine %d: %s", coro_id_, e.what());
        std::vector<VariantType> err;
        err.emplace_back(-1);
        err.emplace_back((std::string("Variant access error: ") + e.what()));
        return err;
    } catch (const std::exception& e) {
        xlog_err("Exception in await_resume for coroutine %d: %s", coro_id_, e.what());
        std::vector<VariantType> err;
        err.emplace_back(-1);
        err.emplace_back((std::string("Exception: ") + e.what()));
        return err;
    } catch (...) {
        xlog_err("Unknown exception in await_resume for coroutine %d", coro_id_);
        std::vector<VariantType> err;
        err.emplace_back(-1);
        err.emplace_back(std::string("Unknown exception"));
        return err;
    }
}

// ==============================================
// External interfaces
// ==============================================

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

static void coroutine_wait_timeout(void* data) {
    uint32_t wait_id = (uint32_t)data;
    if (_co_svs) _co_svs->resume_waiter_timeout(wait_id);
}

void* coroutine_timer(uint32_t wait_id, int time_ms) {
    if (time_ms < 10) time_ms = 10;

    char name[32];
    snprintf(name, sizeof(name), "coro:wait:%d", wait_id);
    return (void*)xtimer_add(time_ms, name, coroutine_wait_timeout, (void*)wait_id, 0);
}

static void coroutine_weekup(void* data) {
    uint32_t coro_id = (uint32_t)data;
    if (_co_svs) _co_svs->resume_coroutine(coro_id, NULL);
}

xAwaiter coroutine_sleep(int time_ms) {
    if (_co_svs) {
        xAwaiter awaiter;
        awaiter.set_timeout(time_ms);
        return awaiter;
    } else {
        return xAwaiter(0);
    }
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

bool coroutine_resume(uint32_t wait_id, std::vector<VariantType> && resp) {
    if (!_co_svs) return false;
    _co_svs->resume_waiter(wait_id, std::move(resp));
    return true;
}

void coroutine_set_stacktrace_mode(int mode) {
    switch (mode) {
    case 0:
        xtraceback_auto_detect();
        break;
    case 1:
        xtraceback_force_simple();
        break;
    case 2:
        xtraceback_force_detailed();
        break;
    default:
        xlog_err("Invalid stack trace mode: %d, using auto detect", mode);
        xtraceback_auto_detect();
        break;
    }
}
