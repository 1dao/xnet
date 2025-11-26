// xcoroutine.cpp

#include "xcoroutine.h"
#include <coroutine>
#include <unordered_map>
#include <memory>
#include <iostream>
#include <atomic>

#include "xmutex.h"

// 前向声明
class xCoroService;

// 单例实例
static xCoroService* _co_svs = nullptr;

// 线程局部存储
static thread_local int _co_cid = -1;

// 协程包装器
struct xCoro {
    xTask task;
    int coroutine_id;

    xCoro(xTask&& t, int id) : task(std::move(t)), coroutine_id(id) {
        task.get_promise().coroutine_id = id;
    }
    ~xCoro() = default;
    xCoro(const xCoro&) = delete;
    xCoro& operator=(const xCoro&) = delete;

    bool is_done() const { return task.done(); }
    void resume(void* param) {
        if (!is_done()) {
            _co_cid = coroutine_id;
            task.resume(param);
            _co_cid = -1;
        }
    }
    int get_id() const { return coroutine_id; }
};

// Mutex RAII 包装器
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

// 协程管理器实现
class xCoroService {
public:
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

    xCoro* find_coroutine_by_id(int id) {
        XMutexGuard lock(&map_mutex);
        auto it = coroutine_map_.find(id);
        return (it != coroutine_map_.end()) ? it->second.get() : nullptr;
    }

    int run_task(fnCoro func, void* arg) {
        int coro_id = generate_coroutine_id();
        int old_id = _co_cid;
        _co_cid = coro_id;

        xTask task = func(arg);

        _co_cid = old_id;

        if (task.done()) {
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
            coro->resume(param);
            if (coro->is_done()) {
                remove_coroutine(id);
            }
            return true;
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
            if (coro && !coro->is_done()) coro->resume(nullptr);
        }
    }

    size_t get_active_count() const {
        XMutexGuard lock(&map_mutex);
        return coroutine_map_.size();
    }

    // -------------------- Wait 相关 --------------------
    void register_waiter(uint32_t wait_id, std::coroutine_handle<> h, int coro_id) {
        std::coroutine_handle<> to_resume = nullptr;
        int resume_coro_id = -1;
        {
            XMutexGuard lock(&wait_mutex);
            auto& p = wait_map_[wait_id];
            p.handle = h;
            p.coro_id = coro_id;  // 保存协程ID
            if (p.done && p.result) {
                to_resume = p.handle;
                resume_coro_id = p.coro_id;
            }
        }
        if (to_resume) {
            _co_cid = resume_coro_id;  // 恢复协程ID
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
                resume_coro_id = p.coro_id;  // 获取协程ID
            }
        }
        if (to_resume) {
            _co_cid = resume_coro_id;  // 恢复协程ID
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

// -------------------- Awaiter 实现 --------------------
std::coroutine_handle<> xFinAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    if (_co_svs && coroutine_id > 0) {
        _co_svs->remove_coroutine(coroutine_id);
    }
    return std::noop_coroutine();
}

xAwaiter::xAwaiter() noexcept
    : wait_id_(_co_svs ? _co_svs->generate_wait_id() : 0)
    , error_code_(0)
    , coro_id_(_co_cid) {  // 保存当前协程ID
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

// -------------------- 外部接口 --------------------
bool coroutine_init() {
    if (_co_svs) return true;
    try {
        _co_svs = new xCoroService();
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
