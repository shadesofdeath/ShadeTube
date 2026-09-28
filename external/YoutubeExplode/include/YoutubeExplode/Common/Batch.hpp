#pragma once

#include <functional>
#include <vector>

namespace YoutubeExplode::Common {

/// Generic collection of items returned by a single paginated request.
template <typename T>
class Batch {
public:
    Batch() = default;
    explicit Batch(std::vector<T> items) : items_(std::move(items)) {}

    const std::vector<T>& items() const noexcept { return items_; }

private:
    std::vector<T> items_;
};

/// Callback invoked for every batch of a paginated result.
/// Return false to stop fetching further pages (equivalent to breaking out of `await foreach`).
template <typename T>
using BatchHandler = std::function<bool(const Batch<T>&)>;

} // namespace YoutubeExplode::Common
