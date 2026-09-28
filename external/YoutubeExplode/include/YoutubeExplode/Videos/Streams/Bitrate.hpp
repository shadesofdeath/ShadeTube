#pragma once

#include <string>

namespace YoutubeExplode::Videos::Streams {

/// Encoded bitrate of a stream.
class Bitrate {
public:
    Bitrate() = default;
    explicit Bitrate(long long bitsPerSecond) : bps_(bitsPerSecond) {}

    long long bitsPerSecond() const noexcept { return bps_; }
    double kiloBitsPerSecond() const noexcept { return static_cast<double>(bps_) / 1024.0; }
    double megaBitsPerSecond() const noexcept { return kiloBitsPerSecond() / 1024.0; }
    double gigaBitsPerSecond() const noexcept { return megaBitsPerSecond() / 1024.0; }

    /// E.g. "128.5 Kbit/s".
    std::string toString() const;

    friend bool operator==(Bitrate a, Bitrate b) { return a.bps_ == b.bps_; }
    friend bool operator!=(Bitrate a, Bitrate b) { return a.bps_ != b.bps_; }
    friend bool operator<(Bitrate a, Bitrate b) { return a.bps_ < b.bps_; }
    friend bool operator>(Bitrate a, Bitrate b) { return a.bps_ > b.bps_; }

private:
    long long bps_ = 0;
};

} // namespace YoutubeExplode::Videos::Streams
