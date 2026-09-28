// 烛龙 (ZhuLong) - a worker pool for the software renderer.
//
// The pool is owned by the backend and not by a draw: spawning threads per draw
// would cost more than the draws themselves. `Run` makes the calling thread one of
// the workers, so a machine with two cores is not left with one core idle while the
// other waits.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace zlong::gpu::software {

class ThreadPool {
public:
    /// `workers` background threads, so total parallelism is workers + 1.
    explicit ThreadPool(std::size_t workers);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    /// Total parallelism, including the calling thread.
    std::size_t parallelism() const noexcept { return workers_.size() + 1; }

    /// Run body(task) for every task in [0, tasks), on this thread and the workers,
    /// and return when all of them are done. Not reentrant.
    void Run(std::size_t tasks, const std::function<void(std::size_t)>& body);

private:
    void Worker();

    std::vector<std::thread> workers_;
    std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable done_;

    /// Valid only while a Run is in flight; guarded by `mutex_`.
    const std::function<void(std::size_t)>* body_ = nullptr;
    std::size_t task_count_ = 0;
    /// Claimed and counted without the lock: these are the hot path.
    std::atomic<std::size_t> next_{0};
    std::atomic<std::size_t> completed_{0};
    bool stopping_ = false;
};

}  // namespace zlong::gpu::software
