#pragma once
// Background work -> UI-thread continuation, with lifetime safety.
//
//   class Page { st::Lifetime life_; ... };
//   st::async(st::Priority::High, life_.ref(),
//       [id] { return mb::album(id);    },                 // worker thread
//       [this](st::Result<Playlist> r) { if (r) show(*r); });   // UI thread, only if `this` still alive
//
// If the guard expires (owner destroyed or life_.renew() called on navigation) BEFORE the work
// starts, the work is skipped entirely - stale requests never hit the network. If it expires
// afterwards, the continuation is dropped.
#include "core/Dispatcher.h"
#include "core/ThreadPool.h"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace st {

struct Unit {};

class Lifetime {
public:
    using Ref = std::weak_ptr<const void>;
    Lifetime() : token_(std::make_shared<char>(char{})) {}
    Lifetime(const Lifetime&) = delete;
    Lifetime& operator=(const Lifetime&) = delete;

    Ref ref() const { return token_; }
    // Invalidates every outstanding callback (e.g. a page navigates to other content).
    void renew() { token_ = std::make_shared<char>(char{}); }

private:
    std::shared_ptr<const void> token_;
};

template <class T>
class Result {
public:
    Result() = default;
    static Result success(T v) { Result r; r.value_.emplace(std::move(v)); return r; }
    static Result failure(std::exception_ptr e) { Result r; r.error_ = std::move(e); return r; }

    explicit operator bool() const noexcept { return value_.has_value(); }
    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }
    T* operator->() { return &*value_; }
    const T* operator->() const { return &*value_; }
    std::exception_ptr error() const { return error_; }

    std::string errorMessage() const {
        if (!error_) return {};
        try { std::rethrow_exception(error_); }
        catch (const std::exception& e) { return e.what(); }
        catch (...) { return "unknown error"; }
    }

private:
    std::optional<T> value_;
    std::exception_ptr error_;
};

template <class Work, class Done>
void async(Priority priority, Lifetime::Ref guard, Work&& work, Done&& done) {
    using Raw = std::invoke_result_t<Work>;
    using T = std::conditional_t<std::is_void_v<Raw>, Unit, Raw>;
    ThreadPool::shared().post(
        [guard, work = std::forward<Work>(work), done = std::forward<Done>(done)]() mutable {
            if (guard.expired()) return;
            Result<T> result;
            try {
                if constexpr (std::is_void_v<Raw>) {
                    work();
                    result = Result<T>::success(Unit{});
                } else {
                    result = Result<T>::success(work());
                }
            } catch (...) {
                result = Result<T>::failure(std::current_exception());
            }
            Dispatcher::post([guard, done = std::move(done), result = std::move(result)]() mutable {
                if (!guard.expired()) done(std::move(result));
            });
        },
        priority);
}

// Fire-and-forget background work (no continuation).
template <class Work>
void background(Priority priority, Work&& work) {
    ThreadPool::shared().post(std::forward<Work>(work), priority);
}

} // namespace st
