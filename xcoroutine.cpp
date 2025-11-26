// xcoroutine.cpp

#include "xcoroutine.h"
#include <coroutine>
#include <unordered_map>
#include <memory>
#include <functional>
#include <iostream>
#include <exception>
#include <vector>
#include <atomic>
#include <optional>
#include <thread>
#include <chrono>

#include "xmutex.h"

// 前向声明
class xCoroService;

// 单例实例
static xCoroService* _coro_svs = nullptr;

// 线程局部存储，用于存储当前正在执行的协程ID
static thread_local int g_current_coroutine_id = -1;

// 协程包装器
struct xCoro {
    xTask task;
    int coroutine_id;

    xCoro(xTask&& t, int id)
        : task(std::move(t)), coroutine_id(id) {
        task.get_promise().coroutine_id = id;
        std::cout << "xCoro created, ID: " << id << std::endl;
    }

    ~xCoro() {
        std::cout << "xCoro destroyed, ID: " << coroutine_id << std::endl;
    }

    // 禁用拷贝
    xCoro(const xCoro&) = delete;
    xCoro& operator=(const xCoro&) = delete;

    bool is_done() const {
        return task.done();
    }

    void resume(void* param) {
        if (!is_done()) {
            g_current_coroutine_id = coroutine_id;
            task.resume(param);
            g_current_coroutine_id = -1;
        }
    }

    int get_id() const {
        return coroutine_id;
    }
};

// xmutex RAII 包装器
class XMutexGuard {
public:
    explicit XMutexGuard(xMutex* mutex) : m_mutex(mutex) {
        if (m_mutex) {
            xnet_mutex_lock(m_mutex);
        }
    }

    ~XMutexGuard() {
        if (m_mutex) {
            xnet_mutex_unlock(m_mutex);
        }
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
    mutable xMutex rpc_mutex;
    std::atomic<int> next_coroutine_id_{ 0 };

    // RPC map
    struct PendingRPC {
        std::coroutine_handle<> handle = nullptr;
        std::unique_ptr<std::vector<VariantType>> result;
        bool done = false;
    };
    std::unordered_map<uint32_t, PendingRPC> rpc_map;

    int generate_coroutine_id() {
        return ++next_coroutine_id_;
    }

public:
    xCoroService() {
        xnet_mutex_init(&map_mutex);
        xnet_mutex_init(&rpc_mutex);
    }

    ~xCoroService() {
        {
            XMutexGuard lock(&map_mutex);
            coroutine_map_.clear();
        }
        {
            XMutexGuard lock(&rpc_mutex);
            rpc_map.clear();
        }
        xnet_mutex_uninit(&map_mutex);
        xnet_mutex_uninit(&rpc_mutex);
    }

    // 查找协程（返回原始指针）
    xCoro* find_coroutine_by_id(int coroutine_id) {
        XMutexGuard lock(&map_mutex);
        auto it = coroutine_map_.find(coroutine_id);
        if (it != coroutine_map_.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    // 运行协程
    int run_task(fnCoro func, void* arg) {
        int coroutine_id = generate_coroutine_id();

        // 设置线程局部变量
        int old_coro_id = g_current_coroutine_id;
        g_current_coroutine_id = coroutine_id;

        // 创建并执行协程（initial_suspend 返回 suspend_never，立即执行）
        xTask task = func(arg);

        // 恢复之前的协程 ID
        g_current_coroutine_id = old_coro_id;

        // 检查协程是否已经执行完成
        if (task.done()) {
            // 协程已经立即完成，不需要加入 map
            std::cout << "Coroutine " << coroutine_id
                      << " completed immediately, no need to track" << std::endl;
            return coroutine_id;
        }

        // 协程在某处挂起了，需要加入 map 进行管理
        task.get_promise().coroutine_id = coroutine_id;

        XMutexGuard guard(&map_mutex);
        coroutine_map_[coroutine_id] = std::make_unique<xCoro>(std::move(task), coroutine_id);

        std::cout << "Coroutine " << coroutine_id
                  << " is suspended, added to map for tracking" << std::endl;

        return coroutine_id;
    }

    // 通过ID恢复特定协程
    bool resume_coroutine(int coroutine_id, void* param) {
        xCoro* coroutine = find_coroutine_by_id(coroutine_id);
        if (!coroutine) {
            std::cout << "Coroutine ID " << coroutine_id << " not found" << std::endl;
            return false;
        }

        if (!coroutine->is_done()) {
            coroutine->resume(param);
            if (coroutine->is_done()) {
                remove_coroutine(coroutine_id);
            }
            return true;
        }
        remove_coroutine(coroutine_id);
        return false;
    }

    // 移除协程
    bool remove_coroutine(int coroutine_id) {
        XMutexGuard lock(&map_mutex);
        auto it = coroutine_map_.find(coroutine_id);
        if (it != coroutine_map_.end()) {
            std::cout << "Removing coroutine, ID: " << coroutine_id
                      << ", remaining: " << (coroutine_map_.size() - 1) << std::endl;
            coroutine_map_.erase(it);
            return true;
        }
        return false;
    }

    // 检查协程状态
    bool is_coroutine_done(int coroutine_id) {
        xCoro* coroutine = find_coroutine_by_id(coroutine_id);
        if (coroutine) {
            return coroutine->is_done();
        }
        return true;
    }

    // 恢复所有可运行的协程
    void resume_all() {
        std::vector<int> ids;

        {
            XMutexGuard lock(&map_mutex);
            ids.reserve(coroutine_map_.size());
            for (auto& [id, coro] : coroutine_map_) {
                if (!coro->is_done()) {
                    ids.push_back(id);
                }
            }
        }

        for (int id : ids) {
            xCoro* coro = find_coroutine_by_id(id);
            if (coro && !coro->is_done()) {
                coro->resume(nullptr);
            }
        }
    }

    // 获取活跃协程数量
    size_t get_active_count() const {
        XMutexGuard lock(&map_mutex);
        return coroutine_map_.size();
    }

    // -------------------- RPC 相关 --------------------
    void register_rpc_waiter(uint32_t pkg_id, std::coroutine_handle<> h) {
        std::coroutine_handle<> to_resume = nullptr;

        {
            XMutexGuard lock(&rpc_mutex);

            auto& p = rpc_map[pkg_id];
            p.handle = h;

            if (p.done && p.result) {
                to_resume = p.handle;
            }
        }

        if (to_resume) {
            to_resume.resume();
        }
    }

    void resume_rpc(uint32_t pkg_id, std::vector<VariantType>&& resp) {
        std::coroutine_handle<> to_resume = nullptr;

        {
            XMutexGuard lock(&rpc_mutex);

            auto& p = rpc_map[pkg_id];
            p.result = std::make_unique<std::vector<VariantType>>(std::move(resp));
            p.done = true;

            if (p.handle) {
                to_resume = p.handle;
            }
        }

        if (to_resume) {
            to_resume.resume();
        }
    }

    std::vector<VariantType> take_rpc_result(uint32_t pkg_id) {
        XMutexGuard lock(&rpc_mutex);

        auto it = rpc_map.find(pkg_id);
        if (it == rpc_map.end() || !it->second.result)
            return {};

        auto r = std::move(*it->second.result);
        rpc_map.erase(it);
        return r;
    }
};

// -------------------- FinalAwaiter 实现 --------------------
std::coroutine_handle<> xFinAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    if (_coro_svs && coroutine_id > 0) {
        std::cout << "FinalAwaiter: Cleaning up coroutine " << coroutine_id << std::endl;
        _coro_svs->remove_coroutine(coroutine_id);
    }
    return std::noop_coroutine();
}

// -------------------- xAwaiter 实现 --------------------
void xAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    if (!_coro_svs) return;
    _coro_svs->register_rpc_waiter(pkg_id_, h);
}

std::vector<VariantType> xAwaiter::await_resume() noexcept {
    if (!_coro_svs) return {};
    return _coro_svs->take_rpc_result(pkg_id_);
}

// -------------------- 外部接口实现 --------------------
bool coroutine_init() {
    if (_coro_svs != nullptr) {
        return true;
    }

    try {
        _coro_svs = new xCoroService();
        std::cout << "Coroutine manager initialized" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to initialize coroutine manager: " << e.what() << std::endl;
        return false;
    }
}

void coroutine_uninit() {
    if (_coro_svs != nullptr) {
        delete _coro_svs;
        _coro_svs = nullptr;
        std::cout << "Coroutine manager uninitialized" << std::endl;
    }
}

int coroutine_run(fnCoro func, void* arg) {
    if (!_coro_svs) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return -1;
    }
    return _coro_svs->run_task(func, arg);
}

bool coroutine_resume(int coroutine_id, void* param) {
    if (!_coro_svs) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return false;
    }
    return _coro_svs->resume_coroutine(coroutine_id, param);
}

void coroutine_resume_all() {
    if (!_coro_svs) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return;
    }
    _coro_svs->resume_all();
}

bool coroutine_is_done(int coroutine_id) {
    if (!_coro_svs) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return true;
    }
    return _coro_svs->is_coroutine_done(coroutine_id);
}

size_t coroutine_get_active_count() {
    if (!_coro_svs) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return 0;
    }
    return _coro_svs->get_active_count();
}

int coroutine_self_id() {
    return g_current_coroutine_id;
}

bool coroutine_resume(uint32_t pkg_id, std::vector<VariantType>&& resp) {
    if (!_coro_svs) return false;
    _coro_svs->resume_rpc(pkg_id, std::move(resp));
    return true;
}
