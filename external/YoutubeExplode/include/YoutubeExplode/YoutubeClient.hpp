#pragma once

#include <YoutubeExplode/Channels/ChannelClient.hpp>
#include <YoutubeExplode/Http/HttpClient.hpp>
#include <YoutubeExplode/JavaScript/IJsEngine.hpp>
#include <YoutubeExplode/Playlists/PlaylistClient.hpp>
#include <YoutubeExplode/Search/SearchClient.hpp>
#include <YoutubeExplode/Videos/VideoClient.hpp>
#include <YoutubeExplode/detail/Forward.hpp>

#include <memory>
#include <string>
#include <vector>

namespace YoutubeExplode {

/// Configuration of a YoutubeClient. Every field is optional.
struct YoutubeClientOptions {
    /// Transport. Defaults to createDefaultHttpClient() (WinHTTP on Windows, libcurl elsewhere).
    std::shared_ptr<Http::IHttpClient> httpClient;

    /// Cookies of a signed-in account (e.g. exported from a browser). They enable access to
    /// age-restricted and private content and raise rate limits. The SAPISID / __Secure-3PAPISID
    /// cookie is used to generate the SAPISIDHASH authorization header automatically.
    std::vector<Http::Cookie> initialCookies;

    /// Optional JavaScript engine used to resolve the "n" throttling parameter (and as a
    /// fallback for the signature cipher). See IJsEngine for details.
    std::shared_ptr<JavaScript::IJsEngine> jsEngine;

    /// When true, each stream URL is probed (last byte range request) and dropped from the
    /// manifest if YouTube rejects it, exactly like the C# library does. This costs one extra
    /// request per stream; disabled by default.
    bool validateStreamUrls = false;

    /// Interface language ("hl") and region ("gl") sent to YouTube.
    std::string language = "en";
    std::string region = "US";
};

/// Client for interacting with YouTube. Cheap to copy; copies share the same session
/// (cookies, visitor data, cached player cipher). Thread-safe.
///
///   YoutubeExplode::YoutubeClient youtube;
///   auto video    = youtube.videos().get("https://youtube.com/watch?v=u_yIGGhubZs");
///   auto manifest = youtube.videos().streams().getManifest(video.id());
///   auto audio    = manifest.tryGetBestAudioOnlyStream();
class YoutubeClient {
public:
    YoutubeClient();
    explicit YoutubeClient(YoutubeClientOptions options);
    explicit YoutubeClient(std::vector<Http::Cookie> initialCookies);
    explicit YoutubeClient(std::shared_ptr<Http::IHttpClient> httpClient);

    /// Operations related to YouTube videos (and, through it, streams and closed captions).
    const Videos::VideoClient& videos() const noexcept { return videos_; }
    /// Operations related to YouTube playlists.
    const Playlists::PlaylistClient& playlists() const noexcept { return playlists_; }
    /// Operations related to YouTube channels.
    const Channels::ChannelClient& channels() const noexcept { return channels_; }
    /// Operations related to YouTube search.
    const Search::SearchClient& search() const noexcept { return search_; }

    // ---- Convenience shortcuts ------------------------------------------------------------

    /// Same as videos().get(videoId).
    Videos::Video getVideo(const Videos::VideoId& videoId, const CancellationToken& ct = {}) const {
        return videos_.get(videoId, ct);
    }
    /// Same as videos().streams().getManifest(videoId).
    Videos::Streams::StreamManifest getStreamManifest(const Videos::VideoId& videoId,
                                                      const CancellationToken& ct = {}) const {
        return videos_.streams().getManifest(videoId, ct);
    }
    /// Same as search().getVideos(query, maxCount).
    std::vector<Search::VideoSearchResult> searchVideos(const std::string& query, std::size_t maxCount = 20,
                                                        const CancellationToken& ct = {}) const {
        return search_.getVideos(query, maxCount, ct);
    }

private:
    detail::ClientContextPtr context_;
    Videos::VideoClient videos_;
    Playlists::PlaylistClient playlists_;
    Channels::ChannelClient channels_;
    Search::SearchClient search_;
};

/// Short alias.
using Client = YoutubeClient;

} // namespace YoutubeExplode
