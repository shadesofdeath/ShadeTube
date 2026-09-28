#pragma once

#include <YoutubeExplode/Common/Cancellation.hpp>
#include <YoutubeExplode/Videos/Streams/StreamInfo.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <cstddef>
#include <string>

namespace YoutubeExplode::Videos::Streams {

/// Readable, seekable stream over a YouTube media stream.
///
/// Mirrors YoutubeExplode's MediaStream: the content is fetched in ~10 MB segments using the
/// `range` query parameter, which avoids YouTube's per-connection rate throttling and provides
/// seeking and automatic retries.
class MediaStream {
public:
    MediaStream(detail::ClientContextPtr context, std::string url, long long length,
                CancellationToken cancellationToken = {});

    /// Reads up to `count` bytes. Returns 0 at the end of the stream.
    std::size_t read(char* buffer, std::size_t count);

    enum class SeekOrigin { Begin, Current, End };
    long long seek(long long offset, SeekOrigin origin = SeekOrigin::Begin);

    long long length() const noexcept { return length_; }
    long long position() const noexcept { return position_; }
    void setPosition(long long value) noexcept { position_ = value; }

private:
    void loadSegment();

    detail::ClientContextPtr context_;
    std::string url_;
    long long length_;
    long long segmentLength_;
    long long position_ = 0;
    CancellationToken cancellationToken_;

    std::string segment_;
    long long segmentStart_ = -1;
};

} // namespace YoutubeExplode::Videos::Streams
