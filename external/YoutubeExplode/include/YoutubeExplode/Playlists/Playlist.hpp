#pragma once

#include <YoutubeExplode/Common/Author.hpp>
#include <YoutubeExplode/Common/Thumbnail.hpp>
#include <YoutubeExplode/Playlists/PlaylistId.hpp>
#include <YoutubeExplode/Videos/Video.hpp>

#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::Playlists {

/// Properties shared by playlist metadata resolved from different sources.
class IPlaylist {
public:
    virtual ~IPlaylist() = default;
    virtual const PlaylistId& id() const = 0;
    virtual std::string url() const = 0;
    virtual const std::string& title() const = 0;
    /// Empty for system playlists (e.g. mixes, "Liked videos").
    virtual const std::optional<Common::Author>& author() const = 0;
    virtual const std::vector<Common::Thumbnail>& thumbnails() const = 0;
};

/// Metadata associated with a YouTube playlist.
class Playlist : public IPlaylist {
public:
    Playlist(PlaylistId id, std::string title, std::optional<Common::Author> author, std::string description,
             std::optional<int> count, std::vector<Common::Thumbnail> thumbnails)
        : id_(std::move(id)), title_(std::move(title)), author_(std::move(author)),
          description_(std::move(description)), count_(count), thumbnails_(std::move(thumbnails)) {}

    const PlaylistId& id() const override { return id_; }
    std::string url() const override { return "https://www.youtube.com/playlist?list=" + id_.value(); }
    const std::string& title() const override { return title_; }
    const std::optional<Common::Author>& author() const override { return author_; }
    const std::string& description() const noexcept { return description_; }
    /// Total number of videos (may be unavailable for some system playlists).
    std::optional<int> count() const noexcept { return count_; }
    const std::vector<Common::Thumbnail>& thumbnails() const override { return thumbnails_; }

    std::string toString() const { return "Playlist (" + title_ + ")"; }

private:
    PlaylistId id_;
    std::string title_;
    std::optional<Common::Author> author_;
    std::string description_;
    std::optional<int> count_;
    std::vector<Common::Thumbnail> thumbnails_;
};

/// Metadata associated with a YouTube video included in a playlist.
class PlaylistVideo : public Videos::IVideo {
public:
    PlaylistVideo(PlaylistId playlistId, Videos::VideoId id, std::string title, Common::Author author,
                  std::optional<TimeSpan> duration, std::vector<Common::Thumbnail> thumbnails)
        : playlistId_(std::move(playlistId)), id_(std::move(id)), title_(std::move(title)),
          author_(std::move(author)), duration_(duration), thumbnails_(std::move(thumbnails)) {}

    const PlaylistId& playlistId() const noexcept { return playlistId_; }
    const Videos::VideoId& id() const override { return id_; }
    std::string url() const override {
        return "https://www.youtube.com/watch?v=" + id_.value() + "&list=" + playlistId_.value();
    }
    const std::string& title() const override { return title_; }
    const Common::Author& author() const override { return author_; }
    const std::optional<TimeSpan>& duration() const override { return duration_; }
    const std::vector<Common::Thumbnail>& thumbnails() const override { return thumbnails_; }

    std::string toString() const { return "Video (" + title_ + ")"; }

private:
    PlaylistId playlistId_;
    Videos::VideoId id_;
    std::string title_;
    Common::Author author_;
    std::optional<TimeSpan> duration_;
    std::vector<Common::Thumbnail> thumbnails_;
};

} // namespace YoutubeExplode::Playlists
