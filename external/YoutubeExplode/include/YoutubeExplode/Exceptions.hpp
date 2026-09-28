#pragma once

#include <stdexcept>
#include <string>

namespace YoutubeExplode::Exceptions {

/// Base exception for all errors raised by the library (mirrors YoutubeExplodeException).
///
/// Hierarchy:
///   YoutubeExplodeException
///   ├── VideoUnplayableException
///   │   ├── VideoUnavailableException        (deleted / private / nonexistent)
///   │   ├── VideoRequiresPurchaseException   (paid content, exposes a preview video ID)
///   │   └── VideoAgeRestrictedException      (age gate could not be bypassed)
///   ├── PlaylistUnavailableException
///   ├── RequestLimitExceededException        (HTTP 429)
///   ├── CipherExtractionException            (player JS changed; cipher/n extraction failed)
///   └── HttpRequestException                 (transport error or unexpected HTTP status)
class YoutubeExplodeException : public std::runtime_error {
public:
    explicit YoutubeExplodeException(const std::string& message) : std::runtime_error(message) {}
};

/// Video cannot be played (for any reason).
class VideoUnplayableException : public YoutubeExplodeException {
public:
    using YoutubeExplodeException::YoutubeExplodeException;
};

/// Video does not exist, is private, or was deleted.
class VideoUnavailableException : public VideoUnplayableException {
public:
    using VideoUnplayableException::VideoUnplayableException;
};

/// Video requires purchase and cannot be played. A free preview may be available.
class VideoRequiresPurchaseException : public VideoUnplayableException {
public:
    VideoRequiresPurchaseException(const std::string& message, std::string previewVideoId)
        : VideoUnplayableException(message), previewVideoId_(std::move(previewVideoId)) {}

    const std::string& previewVideoId() const noexcept { return previewVideoId_; }

private:
    std::string previewVideoId_;
};

/// Video is age-restricted and none of the available clients could bypass the age gate.
/// Supplying cookies of a signed-in, age-verified account usually resolves this.
class VideoAgeRestrictedException : public VideoUnplayableException {
public:
    using VideoUnplayableException::VideoUnplayableException;
};

/// Playlist does not exist, is private, or was deleted.
class PlaylistUnavailableException : public YoutubeExplodeException {
public:
    using YoutubeExplodeException::YoutubeExplodeException;
};

/// YouTube responded with HTTP 429 (Too Many Requests).
class RequestLimitExceededException : public YoutubeExplodeException {
public:
    using YoutubeExplodeException::YoutubeExplodeException;
};

/// The signature cipher or the n-parameter transform could not be extracted from the
/// player JavaScript. This almost always means YouTube changed its player and the
/// extraction heuristics in src/Bridge/Cipher need to be updated.
class CipherExtractionException : public YoutubeExplodeException {
public:
    using YoutubeExplodeException::YoutubeExplodeException;
};

/// Transport-level failure, or a response with an unexpected HTTP status code.
class HttpRequestException : public YoutubeExplodeException {
public:
    explicit HttpRequestException(const std::string& message, int statusCode = 0)
        : YoutubeExplodeException(message), statusCode_(statusCode) {}

    /// HTTP status code, or 0 if the request failed before a response was received.
    int statusCode() const noexcept { return statusCode_; }

private:
    int statusCode_;
};

/// Thrown when an operation observes a canceled CancellationToken.
class OperationCanceledException : public std::runtime_error {
public:
    OperationCanceledException() : std::runtime_error("The operation was canceled.") {}
};

} // namespace YoutubeExplode::Exceptions
