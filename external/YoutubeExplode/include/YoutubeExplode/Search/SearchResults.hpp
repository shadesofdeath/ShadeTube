#pragma once

#include <YoutubeExplode/Channels/Channel.hpp>
#include <YoutubeExplode/Playlists/Playlist.hpp>
#include <YoutubeExplode/Videos/Video.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::Search {

/// Filter applied to a YouTube search query.
enum class SearchFilter {
    /// No filter applied (videos, playlists and channels).
    None,
    /// Only search for videos.
    Video,
    /// Only search for playlists.
    Playlist,
    /// Only search for channels.
    Channel,
};

/// Abstract result returned by a search query.
class ISearchResult {
public:
    virtual ~ISearchResult() = default;
    virtual std::string url() const = 0;
    virtual const std::string& title() const = 0;
    virtual std::string toString() const = 0;
};

/// Metadata associated with a YouTube video returned by a search query.
class VideoSearchResult final : public ISearchResult, public Videos::IVideo {
public:
    VideoSearchResult(Videos::VideoId id, std::string title, Common::Author author, std::optional<TimeSpan> duration,
                      std::vector<Common::Thumbnail> thumbnails)
        : id_(std::move(id)), title_(std::move(title)), author_(std::move(author)), duration_(duration),
          thumbnails_(std::move(thumbnails)) {}

    const Videos::VideoId& id() const override { return id_; }
    std::string url() const override { return "https://www.youtube.com/watch?v=" + id_.value(); }
    const std::string& title() const override { return title_; }
    const Common::Author& author() const override { return author_; }
    /// Empty for live streams.
    const std::optional<TimeSpan>& duration() const override { return duration_; }
    const std::vector<Common::Thumbnail>& thumbnails() const override { return thumbnails_; }
    std::string toString() const override { return "Video (" + title_ + ")"; }

private:
    Videos::VideoId id_;
    std::string title_;
    Common::Author author_;
    std::optional<TimeSpan> duration_;
    std::vector<Common::Thumbnail> thumbnails_;
};

/// Metadata associated with a YouTube playlist returned by a search query.
class PlaylistSearchResult final : public ISearchResult, public Playlists::IPlaylist {
public:
    PlaylistSearchResult(Playlists::PlaylistId id, std::string title, std::optional<Common::Author> author,
                         std::vector<Common::Thumbnail> thumbnails)
        : id_(std::move(id)), title_(std::move(title)), author_(std::move(author)), thumbnails_(std::move(thumbnails)) {}

    const Playlists::PlaylistId& id() const override { return id_; }
    std::string url() const override { return "https://www.youtube.com/playlist?list=" + id_.value(); }
    const std::string& title() const override { return title_; }
    const std::optional<Common::Author>& author() const override { return author_; }
    const std::vector<Common::Thumbnail>& thumbnails() const override { return thumbnails_; }
    std::string toString() const override { return "Playlist (" + title_ + ")"; }

private:
    Playlists::PlaylistId id_;
    std::string title_;
    std::optional<Common::Author> author_;
    std::vector<Common::Thumbnail> thumbnails_;
};

/// Metadata associated with a YouTube channel returned by a search query.
class ChannelSearchResult final : public ISearchResult, public Channels::IChannel {
public:
    ChannelSearchResult(Channels::ChannelId id, std::string title, std::vector<Common::Thumbnail> thumbnails)
        : id_(std::move(id)), title_(std::move(title)), thumbnails_(std::move(thumbnails)) {}

    const Channels::ChannelId& id() const override { return id_; }
    std::string url() const override { return "https://www.youtube.com/channel/" + id_.value(); }
    const std::string& title() const override { return title_; }
    const std::vector<Common::Thumbnail>& thumbnails() const override { return thumbnails_; }
    std::string toString() const override { return "Channel (" + title_ + ")"; }

private:
    Channels::ChannelId id_;
    std::string title_;
    std::vector<Common::Thumbnail> thumbnails_;
};

} // namespace YoutubeExplode::Search
