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
static void coroutine_signal_handler(int sig) {
    if (g_current_lj && g_current_lj->in_protected_call) {
        g_current_lj->sig = sig;
        longjmp(g_current_lj->buf, 1);  // Remove std:: prefix
    } else {
        // Signal in non-protected call, restore default handling
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

// Install signal handlers for Unix/Linux
static void install_signal_handlers() {
    struct sigaction sa;
    sa.sa_handler = coroutine_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NODEFER;

    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGFPE, &sa, nullptr);
    sigaction(SIGILL, &sa, nullptr);
    sigaction(SIGABRT, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    sigaction(SIGTRAP, &sa, nullptr);
}
#else
// Windows: Use structured exception handling
static LONG WINAPI coroutine_exception_handler(PEXCEPTION_POINTERS ExceptionInfo) {
    if (g_current_lj && g_current_lj->in_protected_call) {
        g_current_lj->sig = ExceptionInfo->ExceptionRecord->ExceptionCode;
        longjmp(g_current_lj->buf, 1);  // Remove std:: prefix
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_signal_handlers() {
    // Install vectored exception handler for Windows
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

    // Use hardware exception protection
    if (setjmp(lj->buf) == 0) {
        // Normal execution path
        handle_.resume();
    } else {
        // Hardware exception path - jumped back from exception handler
        handle_.promise().hardware_signal = lj->sig;
    }

    lj->in_protected_call = false;
    g_current_lj = old_lj;
    return true;
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
        install_signal_handlers();
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

            // Check for any type of exception
            if (task.has_any_exception()) {
                std::string error_msg = task.get_exception_message();
                xlog_err("Coroutine %d exception: %s", coroutine_id, error_msg.c_str());
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

    // -------------------- Wait related --------------------
    void register_waiter(uint32_t wait_id, std::coroutine_handle<> h, int coro_id) {
        std::coroutine_handle<> to_resume = nullptr;
        int resume_coro_id = -1;
        {
            XMutexGuard lock(&wait_mutex);
            auto& p = wait_map_[wait_id];
            p.handle = h;
            p.coro_id = coro_id;  // Save coroutine ID
            if (p.done && p.result) {
                to_resume = p.handle;
                resume_coro_id = p.coro_id;
            }
        }
        if (to_resume) {
            _co_cid = resume_coro_id;  // Restore coroutine ID
            to_resume.resume();
            _co_cid = -1;
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
                resume_coro_id = p.coro_id;  // Get coroutine ID
            }
        }
        if (to_resume) {
            _co_cid = resume_coro_id;  // Restore coroutine ID
            to_resume.resume();
            _co_cid = -1;
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
    return _co_svs->take_wait_result(wait_id_);
}

// -------------------- External interfaces --------------------
bool coroutine_init() {
    if (_co_svs) return true;
    try {
        _co_svs = new xCoroService();
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
