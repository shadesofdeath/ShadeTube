#include "core/ThreadPool.h"

#include "core/Log.h"

#include <windows.h>

#include <cassert>
#include <exception>

namespace st {

static ThreadPool* g_shared = nullptr;

ThreadPool& ThreadPool::shared() {
    assert(g_shared && "ThreadPool::setShared was not called");
    return *g_shared;
}
void ThreadPool::setShared(ThreadPool* pool) { g_shared = pool; }

ThreadPool::ThreadPool(unsigned workers) {
    threads_.reserve(workers);
    for (unsigned i = 0; i < workers; ++i) {
        threads_.emplace_back([this, i](std::stop_token stop) {
            SetThreadDescription(GetCurrentThread(), (L"st-worker-" + std::to_wstring(i)).c_str());
            run(stop);
        });
    }
}

ThreadPool::~ThreadPool() {
    for (auto& t : threads_) t.request_stop();
    cv_.notify_all();
    threads_.clear();  // joins
}

void ThreadPool::post(std::function<void()> task, Priority priority) {
    {
        std::lock_guard lock(mutex_);
        lanes_[static_cast<int>(priority)].push_back(std::move(task));
    }
    cv_.notify_one();
}

size_t ThreadPool::pending() const {
    std::lock_guard lock(mutex_);
    return lanes_[0].size() + lanes_[1].size() + lanes_[2].size();
}

void ThreadPool::run(std::stop_token stop) {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            const bool alive = cv_.wait(lock, stop, [&] {
                return !lanes_[0].empty() || !lanes_[1].empty() || !lanes_[2].empty();
            });
            if (!alive) return;
            for (auto& lane : lanes_) {
                if (!lane.empty()) {
                    task = std::move(lane.front());
                    lane.pop_front();
                    break;
                }
            }
        }
        try {
            task();
        } catch (const std::exception& e) {
            ST_LOG_ERROR("pool", "unhandled task exception: {}", e.what());
        } catch (...) {
            ST_LOG_ERROR("pool", "unhandled non-standard task exception");
        }
    }
}

} // namespace st
