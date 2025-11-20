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
    SimpleTask task;
    int coroutine_id;
    CoroutineFunc user_func;
    void* user_arg;

    CoroutineWrapper(SimpleTask&& t, int id, CoroutineFunc func, void* arg)
        : task(std::move(t)), coroutine_id(id), user_func(func), user_arg(arg) {
        // 设置内部promise的协程ID
        task.get_promise().coroutine_id = id;
        g_current_coroutine_id = coroutine_id;
    }

    bool is_done() const override {
        return task.done();
    }

    void resume(void* param) override {
        if (!is_done()) {
            // 保存之前的协程ID（用于嵌套恢复）
            // int previous_id = g_current_coroutine_id;

            // 设置当前协程ID
            g_current_coroutine_id = coroutine_id;
            task.resume(param);

            // 恢复之前的协程ID
            // g_current_coroutine_id = previous_id;
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
    mutable xMutex map_mutex_;
    std::atomic<int> next_coroutine_id_{ 0 };

    // 生成唯一协程ID
    int generate_coroutine_id() {
        return ++next_coroutine_id_;
    }
public:
    CoroutineManagerImpl() {
        xnet_mutex_init(&map_mutex_);
    }

    ~CoroutineManagerImpl() {
        {
            XMutexGuard lock(&map_mutex_);
            std::cout << "Destroying CoroutineManager, cleaning up "
                << coroutine_map_.size() << " coroutines" << std::endl;
            coroutine_map_.clear();
        }
        xnet_mutex_uninit(&map_mutex_);
    }

    // 根据协程ID查找协程包装器
    std::shared_ptr<CoroutineBase> find_coroutine_by_id(int coroutine_id) {
        XMutexGuard lock(&map_mutex_);
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

        // 将void*转换回SimpleTask*
        SimpleTask* user_task = static_cast<SimpleTask*>(task_ptr);

        
        // 创建包装器，移动用户任务的所有权
        auto wrapper = std::make_shared<CoroutineWrapper>(
            std::move(*user_task), coroutine_id, func, arg);

        // 释放用户返回的任务对象
        delete user_task;

        // 保存到map
        {
            XMutexGuard lock(&map_mutex_);
            coroutine_map_[coroutine_id] = wrapper;
        }

        std::cout << "Coroutine started, ID: " << coroutine_id
            << ", total: " << coroutine_map_.size() << std::endl;

        // 注意：这里不调用 resume，因为 initial_suspend 返回 suspend_never
        // 协程会自动开始执行，直到遇到第一个挂起点

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
            return true;
        }

        // 如果已完成，从map中移除
        remove_coroutine(coroutine_id);
        return false;
    }

    // 安全地移除协程
    bool remove_coroutine(int coroutine_id) {
        XMutexGuard lock(&map_mutex_);
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
        XMutexGuard lock(&map_mutex_);
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
            XMutexGuard lock(&map_mutex_);

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
            XMutexGuard lock(&map_mutex_);
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
        XMutexGuard lock(&map_mutex_);
        return coroutine_map_.size();
    }

    int get_cur_id() {
        return next_coroutine_id_;
    }

    // 获取所有活跃协程ID
    std::vector<int> get_active_coroutine_ids() const {
        std::vector<int> ids;
        XMutexGuard lock(&map_mutex_);
        ids.reserve(coroutine_map_.size());
        for (auto& [id, coroutine] : coroutine_map_) {
            ids.push_back(id);
        }
        return ids;
    }
};

// C接口实现
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

    // 将void*转换回SimpleTask*
    SimpleTask* user_task = static_cast<SimpleTask*>(task_ptr);

    // 创建包装器
    auto wrapper = std::make_shared<CoroutineWrapper>(
        std::move(*user_task), coroutine_id, func, variant_args);

    // 释放用户返回的任务对象
    delete user_task;

    // 保存到map
    {
        XMutexGuard lock(&g_manager->map_mutex_);
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
    //return g_current_coroutine_id == -1 ? g_manager->get_cur_id() : g_current_coroutine_id;
    return g_current_coroutine_id;
}
