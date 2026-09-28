#pragma once

#include <string>

namespace YoutubeExplode::Videos::Streams {

/// File size of a stream.
class FileSize {
public:
    FileSize() = default;
    explicit FileSize(long long bytes) : bytes_(bytes) {}

    long long bytes() const noexcept { return bytes_; }
    double kiloBytes() const noexcept { return static_cast<double>(bytes_) / 1024.0; }
    double megaBytes() const noexcept { return kiloBytes() / 1024.0; }
    double gigaBytes() const noexcept { return megaBytes() / 1024.0; }

    /// E.g. "3.42 MB".
    std::string toString() const;

    friend bool operator==(FileSize a, FileSize b) { return a.bytes_ == b.bytes_; }
    friend bool operator!=(FileSize a, FileSize b) { return a.bytes_ != b.bytes_; }
    friend bool operator<(FileSize a, FileSize b) { return a.bytes_ < b.bytes_; }
    friend bool operator>(FileSize a, FileSize b) { return a.bytes_ > b.bytes_; }

private:
    long long bytes_ = 0;
};

} // namespace YoutubeExplode::Videos::Streams
