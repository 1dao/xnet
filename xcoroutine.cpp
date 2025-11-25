// coroutine_manager.cpp

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
class CoroutineManagerImpl;

// 单例实例
static CoroutineManagerImpl* g_manager = nullptr;

// 线程局部存储，用于存储当前正在执行的协程ID
static thread_local int g_current_coroutine_id = -1;

// 协程任务基类
struct CoroutineBase {
    virtual ~CoroutineBase() = default;
    virtual bool is_done() const = 0;
    virtual void resume(void* param) = 0;
    virtual int get_id() const = 0;
};


// 具体协程的包装器
struct CoroutineWrapper : CoroutineBase {
    xTask task;
    int coroutine_id;
    CoroutineFunc user_func;
    void* user_arg;

    CoroutineWrapper(xTask&& t, int id, CoroutineFunc func, void* arg)
        : task(std::move(t)), coroutine_id(id), user_func(func), user_arg(arg) {
        // 设置内部promise的协程ID
        task.get_promise().coroutine_id = id;
    }

    // 构造函数用于新的CoroutineTaskFunc类型（只接受xTask和id）
    CoroutineWrapper(xTask&& t, int id)
        : task(std::move(t)), coroutine_id(id), user_func(nullptr), user_arg(nullptr) {
        // 设置内部promise的协程ID
        task.get_promise().coroutine_id = id;
    }

    bool is_done() const override {
        return task.done();
    }

    void resume(void* param) override {
        if (!is_done()) {
            // 设置当前协程ID
            g_current_coroutine_id = coroutine_id;
            task.resume(param);
            g_current_coroutine_id = -1;
        }
    }

    int get_id() const override {
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

    // 禁用拷贝
    XMutexGuard(const XMutexGuard&) = delete;
    XMutexGuard& operator=(const XMutexGuard&) = delete;

private:
    xMutex* m_mutex;
};

// 协程管理器实现
class CoroutineManagerImpl {
public:
    std::unordered_map<int, std::shared_ptr<CoroutineBase>> coroutine_map_;
    mutable xMutex map_mutex;
    mutable xMutex rpc_mutex;
    std::atomic<int> next_coroutine_id_{ 0 };

    // RPC map
    struct PendingRPC {
        std::coroutine_handle<> handle = nullptr;
        std::unique_ptr<std::vector<VariantType>> result; // 使用 unique_ptr 避免拷贝需求
        bool done = false;
    };
    std::unordered_map<uint32_t, PendingRPC> rpc_map;

    // 生成唯一协程ID
    int generate_coroutine_id() {
        return ++next_coroutine_id_;
    }
public:
    CoroutineManagerImpl() {
        xnet_mutex_init(&map_mutex);
        xnet_mutex_init(&rpc_mutex);
    }

    ~CoroutineManagerImpl() {
        {
            XMutexGuard lock(&map_mutex);
            coroutine_map_.clear();
        }
        {
            XMutexGuard lock(&rpc_mutex);  // 新增
            rpc_map.clear();
        }

        xnet_mutex_uninit(&map_mutex);
        xnet_mutex_uninit(&rpc_mutex);    // 新增
    }

    // 根据协程ID查找协程包装器
    std::shared_ptr<CoroutineBase> find_coroutine_by_id(int coroutine_id) {
        XMutexGuard lock(&map_mutex);
        auto it = coroutine_map_.find(coroutine_id);
        if (it != coroutine_map_.end()) {
            return it->second;
        }
        return nullptr;
    }

    // 启动协程并管理其生命周期，返回协程ID
    int coroutine_run(void* (*func)(void*), void* arg) {
        if (!func) {
            std::cerr << "Invalid function pointer" << std::endl;
            return -1;
        }

        // 生成唯一ID
        int coroutine_id = generate_coroutine_id();
        g_current_coroutine_id = coroutine_id;

        // 调用用户函数获取协程任务
        void* task_ptr = func(arg);
        if (!task_ptr) {
            std::cerr << "User function returned null task" << std::endl;
            return -1;
        }

        // 将void*转换回xTask*
        xTask* user_task = static_cast<xTask*>(task_ptr);

        // 创建包装器，移动用户任务的所有权
        auto wrapper = std::make_shared<CoroutineWrapper>(
            std::move(*user_task), coroutine_id, func, arg);

        // 释放用户返回的任务对象
        delete user_task;

        // 保存到map
        {
            XMutexGuard lock(&map_mutex);
            coroutine_map_[coroutine_id] = wrapper;
        }

        std::cout << "Coroutine started, ID: " << coroutine_id
            << ", total: " << coroutine_map_.size() << std::endl;

        // 注意：这里不调用 resume，因为 initial_suspend 返回 suspend_never
        // 协程会自动开始执行，直到遇到第一个挂起点

        return coroutine_id;
    }

    // 添加新的coroutine_run_task方法
    int coroutine_run_task(CoroutineTaskFunc func, void* arg) {
        XMutexGuard guard(&map_mutex);

        int coroutine_id = generate_coroutine_id();
        g_current_coroutine_id = coroutine_id;

        // 直接使用函数返回的xTask
        xTask task = func(arg);

        auto wrapper = std::make_shared<CoroutineWrapper>(std::move(task), coroutine_id);
        coroutine_map_[coroutine_id] = wrapper;

        return coroutine_id;
    }

    // 通过ID恢复特定协程，并传递参数
    bool resume_coroutine(int coroutine_id, void* param) {
        auto coroutine = find_coroutine_by_id(coroutine_id);
        if (!coroutine) {
            std::cout << "Coroutine ID " << coroutine_id << " not found" << std::endl;
            return false;
        }

        if (!coroutine->is_done()) {
            coroutine->resume(param);
            // 如果已完成，从map中移除
            if (coroutine->is_done()) {
                remove_coroutine(coroutine_id);
            }
            return true;
        }
        // 如果完成则移除并返回 false
        remove_coroutine(coroutine_id);
        return false;
    }

    // 安全地移除协程
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
    bool is_coroutine_done(int coroutine_id) const {
        XMutexGuard lock(&map_mutex);
        auto it = coroutine_map_.find(coroutine_id);
        if (it != coroutine_map_.end()) {
            return it->second->is_done();
        }
        return true; // 如果没找到，认为已完成
    }

    // 检查所有协程状态并清理完成的协程
    void check_coroutines() {
        std::vector<int> to_remove;
        {
            XMutexGuard lock(&map_mutex);

            for (auto it = coroutine_map_.begin(); it != coroutine_map_.end();) {
                if (it->second->is_done()) {
                    to_remove.push_back(it->first);
                    it = coroutine_map_.erase(it);
                }
                else {
                    ++it;
                }
            }
        }

        // 在锁外输出信息
        if (!to_remove.empty()) {
            std::cout << "Cleaned " << to_remove.size() << " completed coroutines" << std::endl;
        }
    }

    // 恢复所有可运行的协程
    void resume_all() {
        std::vector<std::shared_ptr<CoroutineBase>> to_resume;

        {
            XMutexGuard lock(&map_mutex);
            to_resume.reserve(coroutine_map_.size());
            for (auto& [id, coroutine] : coroutine_map_) {
                if (!coroutine->is_done()) {
                    to_resume.push_back(coroutine);
                }
            }
        }

        // 在锁外恢复协程（不传递参数）
        for (auto& coroutine : to_resume) {
            coroutine->resume(nullptr);
        }
    }

    // 获取活跃协程数量
    size_t get_active_count() const {
        XMutexGuard lock(&map_mutex);
        return coroutine_map_.size();
    }

    int get_cur_id() {
        return next_coroutine_id_;
    }

    // 获取所有活跃协程ID
    std::vector<int> get_active_coroutine_ids() const {
        std::vector<int> ids;
        XMutexGuard lock(&map_mutex);
        ids.reserve(coroutine_map_.size());
        for (auto& [id, coroutine] : coroutine_map_) {
            ids.push_back(id);
        }
        return ids;
    }

    // -------------------- RPC 相关 --------------------
    // register a waiter (await_suspend)
    void register_rpc_waiter(uint32_t pkg_id, std::coroutine_handle<> h) {
        std::coroutine_handle<> to_resume = nullptr;
        std::unique_ptr<std::vector<VariantType>> result;

        {
            XMutexGuard lock(&rpc_mutex);

            auto& p = rpc_map[pkg_id];
            p.handle = h;

            if (p.done && p.result) {
                // 移动 result，并删除记录
                // result = std::move(p.result);
                to_resume = p.handle;
            }
        }

        if (to_resume) {
            // 因为 resume 不需要 result（恢复后 await_resume 读取）
            to_resume.resume();
        }
    }

    // network receive -> resume routine
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
            return {};   // error handling 或返回空包

        auto r = std::move(*it->second.result); // XPackBuff move
        rpc_map.erase(it);
        return r;   // 返回 XPackBuff，仍然是 move-only，不复制
    }
};

// ========== xcoroutine_rpc 命名空间桥接实现 ==========
namespace xcoroutine_rpc {
    void register_rpc_waiter(uint32_t pkg_id, std::coroutine_handle<> h) {
        if (!g_manager) return;
        g_manager->register_rpc_waiter(pkg_id, h);
    }
    void resume_rpc(uint32_t pkg_id, std::vector<VariantType>& result) {
        if (!g_manager) return;
        g_manager->resume_rpc(pkg_id, std::move(result));
    }
    std::vector<VariantType> take_rpc_result(uint32_t pkg_id) {
        if (!g_manager) return {};
        return g_manager->take_rpc_result(pkg_id);
    }
}

// -------------------- 外部 C 接口实现 --------------------
bool coroutine_init() {
    if (g_manager != nullptr) {
        return true; // 已经初始化
    }

    try {
        g_manager = new CoroutineManagerImpl();
        std::cout << "Coroutine manager initialized" << std::endl;
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Failed to initialize coroutine manager: " << e.what() << std::endl;
        return false;
    }
}

void coroutine_uninit() {
    if (g_manager != nullptr) {
        delete g_manager;
        g_manager = nullptr;
        std::cout << "Coroutine manager uninitialized" << std::endl;
    }
}

int coroutine_run(void* (*func)(void*), void* arg) {
    if (g_manager == nullptr) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return -1;
    }
    if (!func) {
        std::cerr << "Invalid function pointer" << std::endl;
        return -1;
    }
    return g_manager->coroutine_run(func, arg);
}

int coroutine_run_task(CoroutineTaskFunc func, void* arg) {
    if (!g_manager) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return -1;
    }
    return g_manager->coroutine_run_task(func, arg);
}

int coroutine_run_variant(CoroutineFunc func, const VariantCoroutineArgs& args) {
    if (g_manager == nullptr) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return -1;
    }

    if (!func) {
        std::cerr << "Invalid function pointer" << std::endl;
        return -1;
    }

    // 生成唯一ID
    int coroutine_id = g_manager->generate_coroutine_id();
    g_current_coroutine_id = coroutine_id;

    // 创建参数包装器（在堆上分配）
    VariantCoroutineArgs* variant_args = new VariantCoroutineArgs(args);

    // 调用用户函数，传递参数包装器
    void* task_ptr = func(variant_args);
    if (!task_ptr) {
        std::cerr << "User function returned null task" << std::endl;
        delete variant_args;
        return -1;
    }

    // 将void*转换回xTask*
    xTask* user_task = static_cast<xTask*>(task_ptr);

    // 创建包装器
    auto wrapper = std::make_shared<CoroutineWrapper>(
        std::move(*user_task), coroutine_id, func, variant_args);

    // 释放用户返回的任务对象
    delete user_task;

    // 保存到map
    {
        XMutexGuard lock(&g_manager->map_mutex);
        g_manager->coroutine_map_[coroutine_id] = wrapper;
    }

    std::cout << "Coroutine started, ID: " << coroutine_id
        << ", total: " << g_manager->coroutine_map_.size()
        << ", args: " << args.size() << std::endl;

    // 打印参数（调试用）
    variant_args->print_args();

    return coroutine_id;
}

int coroutine_run_variant(CoroutineFunc func, std::initializer_list<CoroutineArg> args) {
    return coroutine_run_variant(func, VariantCoroutineArgs(args));
}

bool coroutine_resume(int coroutine_id, void* param) {
    if (g_manager == nullptr) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return false;
    }

    return g_manager->resume_coroutine(coroutine_id, param);
}

void coroutine_resume_all() {
    if (g_manager == nullptr) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return;
    }

    g_manager->resume_all();
}

bool coroutine_is_done(int coroutine_id) {
    if (g_manager == nullptr) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return true;
    }

    return g_manager->is_coroutine_done(coroutine_id);
}

size_t coroutine_get_active_count() {
    if (g_manager == nullptr) {
        std::cerr << "Coroutine manager not initialized" << std::endl;
        return 0;
    }

    return g_manager->get_active_count();
}

// 获取当前协程的ID
int coroutine_self_id() {
    return g_current_coroutine_id;
}


bool coroutine_resume(uint32_t pkg_id, std::vector<VariantType>&& resp) {
    if (!g_manager) return false;
    g_manager->resume_rpc(pkg_id, std::move(resp));
    return true;
}


void xAwaiter::await_suspend(std::coroutine_handle<> h) noexcept {
    if (!g_manager) return;
    g_manager->register_rpc_waiter(pkg_id_, h);
}

std::vector<VariantType> xAwaiter::await_resume() noexcept {
    if (!g_manager) return {};
    return g_manager->take_rpc_result(pkg_id_);
}
