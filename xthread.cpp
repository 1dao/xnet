// xthread.cpp - 线程相关

#include "xthread.h"
#include "fmacros.h"
#include "xlog.h"
#include <cstring>
#include <string.h>
#include <assert.h>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/socket.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#endif
#include "zmalloc.h"

// ============================================================================
// 全局状态
// ============================================================================

static xThread*         _threads[XTHR_MAX] = {nullptr};
static xMutex           _lock;
static bool             _init = false;

#ifdef _WIN32
static DWORD            _tls = TLS_OUT_OF_INDEXES;
#define XTHR_COMPLETION_KEY ((ULONG_PTR)-1)
#else
static pthread_key_t    _tls;
static pthread_once_t   _tls_once = PTHREAD_ONCE_INIT;
static void tls_init_once() { pthread_key_create(&_tls, nullptr); }
#endif

// ============================================================================
// TLS
// ============================================================================

static void tls_set(int id) {
#ifdef _WIN32
    TlsSetValue(_tls, (LPVOID)(intptr_t)id);
#else
    pthread_setspecific(_tls, (void*)(intptr_t)id);
#endif
}

static int tls_get() {
#ifdef _WIN32
    return (int)(intptr_t)TlsGetValue(_tls);
#else
    return (int)(intptr_t)pthread_getspecific(_tls);
#endif
}

// ============================================================================
// xThread
// ============================================================================

xThread::xThread(bool xwait_)
    : id(0), name(nullptr), running(false), queue(xwait_), userdata(nullptr),
    handle(0), on_init(nullptr), on_update(nullptr), on_cleanup(nullptr) {
}

xThread::~xThread() {
    if (name) {
        zfree(name);
        name = nullptr;
    }
}

void xThread::set_name(const char* name_) {
    if (name) {
        zfree(name);
        name = nullptr;
    }
    if (name_) {
        size_t len = strlen(name_);
        name = (char*)zmalloc(len + 1);
        strcpy(name, name_);
    }
}

// ============================================================================
// xthrQueue
// ============================================================================

xthrQueue::xthrQueue(bool xwait_) : pending_(0),xwait_(xwait_)  {
#ifdef _WIN32
    iocp_ = nullptr;
#else
    fds_[0] = fds_[1] = -1;
#endif
}

xthrQueue::~xthrQueue() {
    // 清理队列中未处理的任务
    while (!queue_.empty()) {
        queue_.pop();  // 依赖xthrTask的析构函数清理资源
    }
    uninit();
}

bool xthrQueue::init() {
    xnet_mutex_init(&lock_);
    pending_ = 0;
    if (!xwait_) {
#ifdef _WIN32
    iocp_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
    return iocp_ != nullptr;
#else
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds_) != 0) return false;
    fcntl(fds_[0], F_SETFL, O_NONBLOCK);
    fcntl(fds_[1], F_SETFL, O_NONBLOCK);
    return true;
#endif
    } else {
#ifdef _WIN32
        iocp_ = nullptr;            // 等待外部设置
#else
        fds_[0] = fds_[1] = -1;     // 等待外部设置
#endif
        return true;
    }
}

void xthrQueue::uninit() {
    if (xwait_) {
        xnet_mutex_uninit(&lock_);
        return;
    }
#ifdef _WIN32
    if (iocp_) { CloseHandle(iocp_); iocp_ = nullptr; }
#else
    if (fds_[0] >= 0) { close(fds_[0]); close(fds_[1]); fds_[0] = fds_[1] = -1; }
#endif
    xnet_mutex_uninit(&lock_);
}

bool xthrQueue::push(xthrTask&& task) {
    xnet_mutex_lock(&lock_);
    queue_.push(std::move(task));
    bool need_notify = true; //!pending_;
    if (need_notify) {
        pending_ = 1;
    }
    xnet_mutex_unlock(&lock_);

    // 在锁外执行写入，减少锁持有时间
    if (need_notify) {
#ifdef _WIN32
        if (!PostQueuedCompletionStatus(iocp_, 0, XTHR_COMPLETION_KEY, nullptr)){
            DWORD error = GetLastError();
            xlog_err("PostQueuedCompletionStatus failed: error=%d", error);
            xnet_mutex_lock(&lock_);
            pending_ = 0;
            xnet_mutex_unlock(&lock_);
        }
#else
        if(write(fds_[0], "!", 1) < 1){
            xnet_mutex_lock(&lock_);
            pending_ = 0;
            xnet_mutex_unlock(&lock_);
        }
#endif
    }
    return true;
}

std::vector<xthrTask> xthrQueue::pop_all() {
    std::vector<xthrTask> tasks;
    xnet_mutex_lock(&lock_);
    pending_ = 0;
    while (!queue_.empty()) {
        tasks.push_back(std::move(queue_.front()));
        queue_.pop();
    }
    xnet_mutex_unlock(&lock_);
    return tasks;
}

bool xthrQueue::wait(int timeout_ms) {
    if (xwait_) return true;

#ifdef _WIN32
    DWORD timeout = (timeout_ms < 0) ? INFINITE : (DWORD)timeout_ms;
    DWORD trans; ULONG_PTR key; LPOVERLAPPED ov;
    return GetQueuedCompletionStatus(iocp_, &trans, &key, &ov, timeout) != FALSE;
#else
    char buf[64];
    while (recv(fds_[1], buf, sizeof(buf), 0) > 0);

    struct pollfd pfd = { fds_[1], POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms) > 0;
#endif
}

// ============================================================================
// 工作线程函数
// ============================================================================

static int process_tasks(xThread* ctx) {
    int count = 0;
    auto tasks = ctx->queue.pop_all();

    for (auto& task : tasks) {
        switch (task.type) {
            case XTHR_TASK_NORMAL: {
                std::vector<VariantType> result;
                try {
                    result = task.func(ctx, task.args);
                    // 插入成功码到开头
                    result.insert(result.begin(), (int)0);
                } catch (const std::exception& e) {
                    xlog_err("Thread[%d] task error: %s", ctx->id, e.what());
                    result.clear();
                    result.push_back((int)XNET_CORO_EXCEPT);
                } catch (...) {
                    result.clear();
                    result.push_back((int)XNET_UNKNOWN_ERROR);
                }

                // 确保即使在异常情况下也能正确处理回调
                if (task.wait_id != 0) {
                    xThread* source_ctx = xthread_get(task.source_thread);
                    if (source_ctx && source_ctx->running) {
                        try {
                            auto resume_task = xthrTask::make_resume(task.wait_id, std::move(result));
                            resume_task.source_thread = ctx->id;
                            source_ctx->queue.push(std::move(resume_task));
                        } catch (const std::exception& e) {
                            xlog_err("Thread[%d] callback error: %s", ctx->id, e.what());
                        } catch (...) {
                            xlog_err("Thread[%d] unknown callback error", ctx->id);
                        }
                    } else {
                        xlog_err("Source thread %d not found or not running, cannot resume coroutine",
                                    task.source_thread);
                    }
                }
                break;
            }

            case XTHR_TASK_RESUME: {
                // 处理协程恢复任务（必须在协程所在的线程执行）
                try {
                    if (task.wait_id != 0) {
                        coroutine_resume(task.wait_id, std::move(task.args));
                    }
                } catch (const std::exception& e) {
                    xlog_err("Thread[%d] coroutine resume error: %s", ctx->id, e.what());
                } catch (...) {
                    xlog_err("Thread[%d] unknown coroutine resume error", ctx->id);
                }
                break;
            }
        }
        count++;
    }
    return count;
}

#ifdef _WIN32
static unsigned __stdcall worker_func(void* arg) {
#else
static void* worker_func(void* arg) {
#endif
    xThread* ctx = (xThread*)arg;
    tls_set(ctx->id);

    xlog_info("Thread[%d:%s] started", ctx->id, ctx->name);
    if (ctx->name) {
        xlog_set_thread_name(ctx->name);
    } else {
        char name[32];
        snprintf(name, sizeof(name), "THR:%d", ctx->id);
        xlog_set_thread_name(name);
    }

    if (ctx->on_init) ctx->on_init(ctx);

    while (ctx->running) {
        if (ctx->queue.get_xwait()) {
            if (ctx->on_update) ctx->on_update(ctx);
            xlog_warn("Thread[%s] weakup & process tasks", ctx->name);
            process_tasks(ctx);
        } else {
            if (ctx->queue.wait(100)) {
                process_tasks(ctx);
            }
            if (ctx->on_update) ctx->on_update(ctx);
        }
    }

    // 处理剩余任务
    process_tasks(ctx);

    if (ctx->on_cleanup) ctx->on_cleanup(ctx);

    xlog_info("Thread[%d:%s] stopped", ctx->id, ctx->name);

#ifdef _WIN32
    return 0;
#else
    return nullptr;
#endif
}

// ============================================================================
// 全局API
// ============================================================================

bool xthread_init() {
    if (_init) return true;

    xnet_mutex_init(&_lock);
    memset(_threads, 0, sizeof(_threads));

#ifdef _WIN32
    _tls = TlsAlloc();
    if (_tls == TLS_OUT_OF_INDEXES) return false;
#else
    pthread_once(&_tls_once, tls_init_once);
#endif

    _init = true;
    return true;
}

void xthread_uninit() {
    if (!_init) return;

    for (int i = 0; i < XTHR_MAX; i++) {
        if (_threads[i]) xthread_unregister(i);
    }

#ifdef _WIN32
    if (_tls != TLS_OUT_OF_INDEXES) {
        // 清理所有线程的TLS数据
        for (int i = 0; i < XTHR_MAX; i++) {
            if (_threads[i]) {
                TlsSetValue(_tls, nullptr);
            }
        }
        TlsFree(_tls);
        _tls = TLS_OUT_OF_INDEXES;
    }
#else
    // Linux平台清理TLS数据
    for (int i = 0; i < XTHR_MAX; i++) {
        if (_threads[i]) {
            pthread_setspecific(_tls, nullptr);
        }
    }
    // 注意：pthread_key_delete不会自动调用析构函数
    pthread_key_delete(_tls);
#endif

    xnet_mutex_uninit(&_lock);
    _init = false;
}

bool xthread_register(int id, bool xwait_, const char* name,
                      void (*on_init)(xThread*),
                      void (*on_update)(xThread*),
                      void (*on_cleanup)(xThread*)) {
    if (id <= 0 || id >= XTHR_MAX || !_init) return false;

    xnet_mutex_lock(&_lock);
    if (_threads[id]) { xnet_mutex_unlock(&_lock); return false; }

    xThread* ctx = new xThread(xwait_);
    ctx->id = id;
    ctx->set_name(name);
    ctx->on_init = on_init;
    ctx->on_update = on_update;
    ctx->on_cleanup = on_cleanup;
    ctx->running = true;
    if (!ctx->queue.init()) {
        delete ctx;
        xnet_mutex_unlock(&_lock);
        return false;
    }

    _threads[id] = ctx;

#ifdef _WIN32
    ctx->handle = (HANDLE)_beginthreadex(nullptr, 0, worker_func, ctx, 0, nullptr);
    if (!ctx->handle) {
        ctx->queue.uninit();
        delete ctx;
        _threads[id] = nullptr;
        xnet_mutex_unlock(&_lock);
        return false;
    }
#else
    if (pthread_create(&ctx->handle, nullptr, worker_func, ctx) != 0) {
        ctx->queue.uninit();
        delete ctx;
        _threads[id] = nullptr;
        xnet_mutex_unlock(&_lock);
        return false;
    }
#endif

    xnet_mutex_unlock(&_lock);
    return true;
}

bool xthread_register_main(int id, bool xwait_, const char* name) {
    if (id <= 0 || id >= XTHR_MAX || !_init) return false;

    xnet_mutex_lock(&_lock);
    if (_threads[id]) { xnet_mutex_unlock(&_lock); return false; }

    xThread* ctx = new xThread(xwait_);
    ctx->id = id;
    ctx->set_name(name);
    ctx->running = true;
    ctx->handle = 0;

    if (!ctx->queue.init()) {
        delete ctx;
        xnet_mutex_unlock(&_lock);
        return false;
    }

    _threads[id] = ctx;
    tls_set(id);

    xnet_mutex_unlock(&_lock);
    return true;
}

void xthread_unregister(int id) {
    if (id <= 0 || id >= XTHR_MAX) return;

    xnet_mutex_lock(&_lock);
    xThread* ctx = _threads[id];
    if (!ctx) { xnet_mutex_unlock(&_lock); return; }

    ctx->running = false;
    _threads[id] = nullptr;
    xnet_mutex_unlock(&_lock);

    if (ctx->handle) {
#ifdef _WIN32
        WaitForSingleObject(ctx->handle, INFINITE);
        CloseHandle(ctx->handle);
#else
        pthread_join(ctx->handle, nullptr);
#endif
    }

    ctx->queue.uninit();
    delete ctx;
}

xThread* xthread_get(int id) {
    if (id <= 0 || id >= XTHR_MAX) return nullptr;
    return _threads[id];
}

int xthread_current_id() { return tls_get(); }

xThread* xthread_current() { return xthread_get(tls_get()); }

int xthread_set_notify(void* fd) {
    xThread* ctx = xthread_current();
    if (!ctx) return -1;

#ifdef _WIN32
    if ((int)(fd) > 0)
        assert(ctx->queue.get_xwait());
    ctx->queue.set_iocp(reinterpret_cast<HANDLE>(fd));
    xlog_warn("xthread_set_notify:%s, %d", ctx->name ? ctx->name : "", fd);
#else
    int fd_int = -1;
    if (fd != nullptr)
        fd_int = static_cast<int>(reinterpret_cast<intptr_t>(fd));
    if (fd_int > 0)
        assert(ctx->queue.get_xwait());
    ctx->queue.set_notify_fd(fd_int);
    xlog_warn("xthread_set_notify:%s, %d", ctx->name ? ctx->name : "", (fd_int));
#endif
    return 0;
}

int xthread_update() {
    xThread* ctx = xthread_current();
    if (!ctx) return 0;

    return process_tasks(ctx);
}

bool xthread_post(int target_id, XThreadFunc func, std::vector<VariantType> args) {
    xThread* target = xthread_get(target_id);
    if (!target || !target->running) return false;

    xthrTask task;
    task.func = std::move(func);
    task.args = std::move(args);
    task.wait_id = 0;
    task.source_thread = tls_get();

    xThread* self = xthread_get(task.source_thread);
    xlog_warn("xthread post msg to:%s-%d, from:%s-%d"
        , target->name ? target->name : "", target->id
        , self->name ? self->name : "", self->id);
    return target->queue.push(std::move(task));
}

// ============================================================================
// RPC调用 - 核心实现
// ============================================================================

xAwaiter xthread_rpc(int target_id, XThreadFunc func, std::vector<VariantType> args) {
    xThread* target = xthread_get(target_id);
    if (!target || !target->running) {
        return xAwaiter(XTHR_ERR_NO_THREAD);
    }

    int co_id = coroutine_self_id();
    if (co_id == -1) {
        return xAwaiter(XNET_NOT_IN_COROUTINE);
    }

    // 创建awaiter获取wait_id
    xAwaiter awaiter;
    uint32_t wait_id = awaiter.wait_id();
    if (wait_id == 0) {
        return xAwaiter(XNET_NOT_IN_COROUTINE);
    }

    // 构造任务
    xthrTask task;
    task.func = std::move(func);
    task.args = std::move(args);
    task.wait_id = wait_id;
    task.source_thread = tls_get();

    // 投递任务
    if (!target->queue.push(std::move(task))) {
        return xAwaiter(XTHR_ERR_QUEUE_FULL);
    }
    return awaiter;
}
