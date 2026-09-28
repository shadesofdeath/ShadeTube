#pragma once

#include <YoutubeExplode/Common/Author.hpp>
#include <YoutubeExplode/Common/Thumbnail.hpp>
#include <YoutubeExplode/Common/Time.hpp>
#include <YoutubeExplode/Videos/VideoId.hpp>

#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::Videos {

/// Properties shared by video metadata resolved from different sources
/// (Video, PlaylistVideo, VideoSearchResult).
class IVideo {
public:
    virtual ~IVideo() = default;

    virtual const VideoId& id() const = 0;
    virtual std::string url() const = 0;
    virtual const std::string& title() const = 0;
    virtual const Common::Author& author() const = 0;
    /// Empty for live streams.
    virtual const std::optional<TimeSpan>& duration() const = 0;
    virtual const std::vector<Common::Thumbnail>& thumbnails() const = 0;
};

/// User activity statistics.
class Engagement {
public:
    Engagement() = default;
    Engagement(long long viewCount, long long likeCount, long long dislikeCount)
        : viewCount_(viewCount), likeCount_(likeCount), dislikeCount_(dislikeCount) {}

    long long viewCount() const noexcept { return viewCount_; }
    long long likeCount() const noexcept { return likeCount_; }
    /// YouTube no longer publishes dislikes; this is 0 for virtually all videos.
    long long dislikeCount() const noexcept { return dislikeCount_; }
    /// Average rating on a 1..5 scale (0 when unavailable).
    double averageRating() const noexcept {
        const auto total = likeCount_ + dislikeCount_;
        return total != 0 ? 1.0 + 4.0 * static_cast<double>(likeCount_) / static_cast<double>(total) : 0.0;
    }

private:
    long long viewCount_ = 0;
    long long likeCount_ = 0;
    long long dislikeCount_ = 0;
};

/// Metadata associated with a YouTube video.
class Video : public IVideo {
public:
    Video(VideoId id, std::string title, Common::Author author, DateTimeOffset uploadDate,
          std::string description, std::optional<TimeSpan> duration,
          std::vector<Common::Thumbnail> thumbnails, std::vector<std::string> keywords,
          Engagement engagement, bool isLive = false)
        : id_(std::move(id)), title_(std::move(title)), author_(std::move(author)), uploadDate_(uploadDate),
          description_(std::move(description)), duration_(duration), thumbnails_(std::move(thumbnails)),
          keywords_(std::move(keywords)), engagement_(engagement), isLive_(isLive) {}

    const VideoId& id() const override { return id_; }
    std::string url() const override { return "https://www.youtube.com/watch?v=" + id_.value(); }
    const std::string& title() const override { return title_; }
    const Common::Author& author() const override { return author_; }
    DateTimeOffset uploadDate() const noexcept { return uploadDate_; }
    const std::string& description() const noexcept { return description_; }
    const std::optional<TimeSpan>& duration() const override { return duration_; }
    const std::vector<Common::Thumbnail>& thumbnails() const override { return thumbnails_; }
    const std::vector<std::string>& keywords() const noexcept { return keywords_; }
    const Engagement& engagement() const noexcept { return engagement_; }
    /// True if the video is a live stream (currently live or upcoming).
    bool isLive() const noexcept { return isLive_; }

    std::string toString() const { return "Video (" + title_ + ")"; }

private:
    VideoId id_;
    std::string title_;
    Common::Author author_;
    DateTimeOffset uploadDate_;
    std::string description_;
    std::optional<TimeSpan> duration_;
    std::vector<Common::Thumbnail> thumbnails_;
    std::vector<std::string> keywords_;
    Engagement engagement_;
    bool isLive_;
};

} // namespace YoutubeExplode::Videos
