#pragma once

#include <atomic>
#include <functional>
#include <memory>

namespace YoutubeExplode {

/// Equivalent of .NET's CancellationToken. A default-constructed token can never be canceled.
class CancellationToken {
public:
    CancellationToken() = default;

    bool isCancellationRequested() const noexcept { return flag_ && flag_->load(); }

    /// Throws OperationCanceledException if cancellation was requested.
    void throwIfCancellationRequested() const;

    static CancellationToken none() { return {}; }

private:
    friend class CancellationTokenSource;
    explicit CancellationToken(std::shared_ptr<std::atomic<bool>> flag) : flag_(std::move(flag)) {}
    std::shared_ptr<std::atomic<bool>> flag_;
};

/// Equivalent of .NET's CancellationTokenSource.
class CancellationTokenSource {
public:
    CancellationTokenSource() : flag_(std::make_shared<std::atomic<bool>>(false)) {}

    void cancel() noexcept { flag_->store(true); }
    bool isCancellationRequested() const noexcept { return flag_->load(); }
    CancellationToken token() const { return CancellationToken(flag_); }

private:
    std::shared_ptr<std::atomic<bool>> flag_;
};

/// Equivalent of .NET's IProgress<double>; receives values in the [0, 1] range.
using Progress = std::function<void(double)>;

} // namespace YoutubeExplode
