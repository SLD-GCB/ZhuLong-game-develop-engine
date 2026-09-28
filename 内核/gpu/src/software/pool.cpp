#include "zlong/gpu/software/pool.h"

namespace zlong::gpu::software {

ThreadPool::ThreadPool(std::size_t workers) {
    workers_.reserve(workers);
    for (std::size_t index = 0; index < workers; ++index) {
        workers_.emplace_back([this] { Worker(); });
    }
}

ThreadPool::~ThreadPool() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    work_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

void ThreadPool::Worker() {
    for (;;) {
        const std::function<void(std::size_t)>* body = nullptr;
        std::size_t count = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            work_.wait(lock, [this] { return stopping_ || body_ != nullptr; });
            if (stopping_) {
                return;
            }
            // Copied under the lock: the caller clears body_/task_count_ once
            // every task has finished, which can be while this loop still runs.
            body = body_;
            count = task_count_;
        }

        for (;;) {
            const std::size_t task = next_.fetch_add(1);
            if (task >= count) {
                break;
            }
            (*body)(task);
            if (completed_.fetch_add(1) + 1 == count) {
                std::lock_guard<std::mutex> lock(mutex_);
                done_.notify_all();
            }
        }
    }
}

void ThreadPool::Run(std::size_t tasks, const std::function<void(std::size_t)>& body) {
    if (tasks == 0) {
        return;
    }
    // No workers, or nothing to share: do not pay for the handshake.
    if (workers_.empty() || tasks == 1) {
        for (std::size_t task = 0; task < tasks; ++task) {
            body(task);
        }
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        body_ = &body;
        task_count_ = tasks;
        next_.store(0);
        completed_.store(0);
    }
    work_.notify_all();

    // The caller takes tasks as well: with one band per core, waiting instead
    // would leave a core idle for the whole job.
    for (;;) {
        const std::size_t task = next_.fetch_add(1);
        if (task >= tasks) {
            break;
        }
        body(task);
        if (completed_.fetch_add(1) + 1 == tasks) {
            std::lock_guard<std::mutex> lock(mutex_);
            done_.notify_all();
        }
    }

    std::unique_lock<std::mutex> lock(mutex_);
    done_.wait(lock, [this, tasks] { return completed_.load() >= tasks; });
    body_ = nullptr;
    task_count_ = 0;
}

}  // namespace zlong::gpu::software
