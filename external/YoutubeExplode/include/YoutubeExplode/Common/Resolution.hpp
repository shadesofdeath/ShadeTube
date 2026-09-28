#pragma once

#include <string>

namespace YoutubeExplode::Common {

/// Resolution of an image or a video.
class Resolution {
public:
    Resolution() = default;
    Resolution(int width, int height) : width_(width), height_(height) {}

    int width() const noexcept { return width_; }
    int height() const noexcept { return height_; }
    long long area() const noexcept { return static_cast<long long>(width_) * height_; }

    std::string toString() const { return std::to_string(width_) + "x" + std::to_string(height_); }

    friend bool operator==(const Resolution& a, const Resolution& b) {
        return a.width_ == b.width_ && a.height_ == b.height_;
    }
    friend bool operator!=(const Resolution& a, const Resolution& b) { return !(a == b); }

private:
    int width_ = 0;
    int height_ = 0;
};

} // namespace YoutubeExplode::Common
