// coroutine.cpp
#include "coroutine.h"
#include <algorithm>
#include <iostream>
#include <windows.h>

class CoroutineTask;
class CoroutineScheduler;

// 协程调度器
class CoroutineScheduler {
private:
    std::vector<CoroutineTask*> tasks;
    std::vector<CoroutineTask*> waiting_tasks;
    bool stopped = false;
    CoroutineTask* cur = nullptr;

public:
    void add_task(CoroutineTask* task);
    void co_wait_read(int fd, CoroutineTask* task, long long timeout_ms = 5000);
    void co_wait_write(int fd, CoroutineTask* task, long long timeout_ms = 5000);
    void run();
    void update();
    void stop() { stopped = true; }
    friend void coroutine_wait_read(int fd, long long timeout_ms);
private:
    void process_waiting_tasks();
};

// 协程任务
class CoroutineTask {
public:
    enum State { READY, RUNNING, WAITING, FINISHED };

    State state = READY;
    std::function<void()> routine;
    int wait_fd = -1;
    int wait_events = 0; // 1=read, 2=write
    long long resume_time = 0;

    CoroutineTask(std::function<void()> func) : routine(func) {}

    void execute(CoroutineScheduler& scheduler) {
        state = RUNNING;
        routine();
        if (state == RUNNING) {
            state = FINISHED;
        }
    }
};
thread_local CoroutineScheduler coroutine_scheduler;

void CoroutineScheduler::add_task(CoroutineTask* task) {
    tasks.push_back(task);
}

void CoroutineScheduler::co_wait_read(int fd, CoroutineTask* task, long long timeout_ms) {
    if (task) {
        task->wait_fd = fd;
        task->wait_events = 1; // 可读
        task->resume_time = coroutine_current_time() + timeout_ms;
        task->state = CoroutineTask::WAITING;
        waiting_tasks.push_back(task);
    }
}

void CoroutineScheduler::co_wait_write(int fd, CoroutineTask* task, long long timeout_ms) {
    task->wait_fd = fd;
    task->wait_events = 2; // 可写
    task->resume_time = coroutine_current_time() + timeout_ms;
    task->state = CoroutineTask::WAITING;
    waiting_tasks.push_back(task);
}

void CoroutineScheduler::update() {
    while (!stopped && (!tasks.empty() || !waiting_tasks.empty())) {
        // 处理等待IO的任务
        if (!waiting_tasks.empty()) {
            process_waiting_tasks();
        }

        // 执行就绪任务
        std::vector<CoroutineTask*> ready_tasks;
        for (auto task : tasks) {
            if (task->state == CoroutineTask::READY) {
                ready_tasks.push_back(task);
            }
        }

        for (auto task : ready_tasks) {
            cur = task;
            task->execute(*this);
            cur = nullptr;

            // 如果任务完成，移除它
            if (task->state == CoroutineTask::FINISHED) {
                auto it = std::find(tasks.begin(), tasks.end(), task);
                if (it != tasks.end()) {
                    tasks.erase(it);
                    delete task; // 清理任务内存
                }
            }
        }
    }
}

void CoroutineScheduler::process_waiting_tasks() {
    fd_set read_fds, write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);

    int max_fd = 0;
    long long now = coroutine_current_time();

    // 设置select的文件描述符
    for (auto it = waiting_tasks.begin(); it != waiting_tasks.end();) {
        CoroutineTask* task = *it;

        // 检查超时
        if (now > task->resume_time) {
            task->state = CoroutineTask::READY;
            it = waiting_tasks.erase(it);
            continue;
        }

        if (task->wait_events & 1) { // 可读
            FD_SET(task->wait_fd, &read_fds);
            if (task->wait_fd > max_fd) max_fd = task->wait_fd;
        }
        if (task->wait_events & 2) { // 可写
            FD_SET(task->wait_fd, &write_fds);
            if (task->wait_fd > max_fd) max_fd = task->wait_fd;
        }

        ++it;
    }

    if (max_fd == 0) return;

    // 设置超时
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 10000; // 10ms

    int result = select(max_fd + 1, &read_fds, &write_fds, NULL, &tv);
    if (result > 0) {
        // 唤醒可读/写的任务
        for (auto it = waiting_tasks.begin(); it != waiting_tasks.end();) {
            CoroutineTask* task = *it;
            bool ready = false;

            if ((task->wait_events & 1) && FD_ISSET(task->wait_fd, &read_fds)) {
                ready = true;
            }
            if ((task->wait_events & 2) && FD_ISSET(task->wait_fd, &write_fds)) {
                ready = true;
            }

            if (ready) {
                task->state = CoroutineTask::READY;
                it = waiting_tasks.erase(it);
            } else {
                ++it;
            }
        }
    }
}

void coroutine_init() {
}

void coroutine_wait_read(int fd, long long timeout_ms) {
    CoroutineTask* task = coroutine_scheduler.cur;
    return coroutine_scheduler.co_wait_read(fd, task, timeout_ms);
}

void coroutine_update() {
    return coroutine_scheduler.update();
}

void coroutine_finish() {
    return coroutine_scheduler.stop();
}

void coroutine_add_task(std::function<void()> func) {
    auto task = new CoroutineTask(func);
    return coroutine_scheduler.add_task(task);
}

long long coroutine_current_time() {
#ifdef _WIN32
    return GetTickCount64();
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}
