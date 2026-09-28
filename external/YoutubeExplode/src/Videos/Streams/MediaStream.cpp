#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Videos/Streams/MediaStream.hpp>

#include "../../ClientContext.hpp"
#include "../../Utils/Url.hpp"

#include <algorithm>
#include <cstring>

namespace YoutubeExplode::Videos::Streams {

namespace {
// YouTube limits the transfer speed of a single request to roughly the playback rate. Splitting the
// download into ~10 MB ranged requests avoids that throttling (same value as the C# library).
constexpr long long kSegmentLength = 9'898'989;
} // namespace

MediaStream::MediaStream(detail::ClientContextPtr context, std::string url, long long length,
                         CancellationToken cancellationToken)
    : context_(std::move(context)), url_(std::move(url)), length_(length),
      segmentLength_(std::min(kSegmentLength, std::max(1LL, length))), cancellationToken_(std::move(cancellationToken)) {}

void MediaStream::loadSegment() {
    const long long from = position_;
    const long long to = std::min(position_ + segmentLength_, length_) - 1;
    Http::HttpRequest request;
    request.url = detail::Url::setQueryParameter(url_, "range", std::to_string(from) + "-" + std::to_string(to));

    for (int retriesRemaining = 5;; --retriesRemaining) {
        try {
            auto response = context_->send(request, cancellationToken_);
            detail::ClientContext::ensureSuccess(response, request.url);
            segment_ = std::move(response.body);
            segmentStart_ = from;
            return;
        } catch (const Exceptions::HttpRequestException& ex) {
            // Retry on connectivity issues and server errors
            const bool transient = ex.statusCode() == 0 || ex.statusCode() >= 500;
            if (!transient || retriesRemaining <= 0)
                throw;
        }
    }
}

std::size_t MediaStream::read(char* buffer, std::size_t count) {
    if (position_ >= length_ || count == 0)
        return 0;
    cancellationToken_.throwIfCancellationRequested();

    const bool inSegment = segmentStart_ >= 0 && position_ >= segmentStart_ &&
                           position_ < segmentStart_ + static_cast<long long>(segment_.size());
    if (!inSegment) {
        loadSegment();
        if (segment_.empty())
            return 0;  // Server returned no data: treat as end of stream.
    }

    const auto offset = static_cast<std::size_t>(position_ - segmentStart_);
    const auto available = segment_.size() - offset;
    const auto n = std::min(count, available);
    std::memcpy(buffer, segment_.data() + offset, n);
    position_ += static_cast<long long>(n);
    return n;
}

long long MediaStream::seek(long long offset, SeekOrigin origin) {
    switch (origin) {
        case SeekOrigin::Begin: position_ = offset; break;
        case SeekOrigin::Current: position_ += offset; break;
        case SeekOrigin::End: position_ = length_ + offset; break;
    }
    if (position_ < 0)
        position_ = 0;
    return position_;
}

} // namespace YoutubeExplode::Videos::Streams
