#pragma once
// Fixed-size worker pool with three priority lanes.
//  High   - work the user is looking at right now (visible page data, visible artwork, playback resolve)
//  Normal - default
//  Low    - speculative work (prefetching the next track, off-screen artwork, cache maintenance)
// Tasks are plain std::function<void()>; exceptions are caught and logged so a worker never dies.
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace st {

enum class Priority { High = 0, Normal = 1, Low = 2 };

class ThreadPool {
public:
    explicit ThreadPool(unsigned workers);
    ~ThreadPool();
    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    void post(std::function<void()> task, Priority priority = Priority::Normal);
    size_t pending() const;

    // Process-wide pool used by services (created by App, destroyed last).
    static ThreadPool& shared();
    static void setShared(ThreadPool* pool);

private:
    void run(std::stop_token stop);

    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<std::function<void()>> lanes_[3];
    std::vector<std::jthread> threads_;
};

} // namespace st
