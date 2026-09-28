#pragma once

#include <string>

namespace YoutubeExplode::Videos::Streams {

/// Stream container (file format), e.g. "mp4", "webm", "m4a".
class Container {
public:
    Container() = default;
    explicit Container(std::string name) : name_(std::move(name)) {}

    const std::string& name() const noexcept { return name_; }

    /// Whether this container is a known audio-only format (mp3, m4a, wav, wma, ogg, aac, opus).
    /// Note that YouTube serves audio-only streams in "mp4" and "webm" containers too.
    bool isAudioOnly() const;

    const std::string& toString() const noexcept { return name_; }

    static Container mp3() { return Container("mp3"); }
    static Container mp4() { return Container("mp4"); }
    static Container webM() { return Container("webm"); }
    static Container tgpp() { return Container("3gpp"); }

    /// Case-insensitive comparison.
    friend bool operator==(const Container& a, const Container& b);
    friend bool operator!=(const Container& a, const Container& b) { return !(a == b); }

private:
    std::string name_;
};

} // namespace YoutubeExplode::Videos::Streams
