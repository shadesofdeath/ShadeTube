#include "ytmusic/YtMusic.h"

#include "core/Http.h"
#include "core/Log.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Music/MusicClient.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace st::ytm {

namespace yte = YoutubeExplode;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

// The filters of YouTube Music's search chips (the "params" its own chips send).
constexpr const char* kSongs = "EgWKAQIIAWoMEA4QChADEAQQCRAF";
constexpr const char* kAlbums = "EgWKAQIYAWoMEA4QChADEAQQCRAF";
constexpr const char* kArtists = "EgWKAQIgAWoMEA4QChADEAQQCRAF";
constexpr const char* kFeaturedPlaylists = "EgeKAQQoADgBagwQDhAKEAMQBBAJEAU%3D";
constexpr const char* kCommunityPlaylists = "EgeKAQQoAEABagwQDhAKEAMQBBAJEAU%3D";

std::atomic<int> g_requests{0}, g_hits{0};

std::mutex g_clientMutex;
std::optional<yte::YoutubeClient> g_client;

yte::YoutubeClient client() {
    std::lock_guard lock(g_clientMutex);
    if (!g_client) {
        yte::YoutubeClientOptions o;
        o.httpClient = http::client();
        g_client.emplace(o);
    }
    return *g_client;
}

// UI language: the client's own (App sets it to the UI language); parsing: English.
enum class Lang { Parse, Ui };

Json send(const std::string& endpoint, Json body, const CT& ct, Lang lang = Lang::Parse, const std::string& query = {}) {
    yte::Music::MusicRequest r;
    r.endpoint = endpoint;
    r.body = body.dump();
    r.query = query;
    if (lang == Lang::Parse) r.language = "en";
    ++g_requests;
    const auto t0 = Clock::now();
    std::string raw;
    try {
        raw = client().music().send(r, ct);
    } catch (const yte::Exceptions::RequestLimitExceededException& e) {
        throw ApiError(429, e.what());
    } catch (const yte::Exceptions::HttpRequestException& e) {
        if (e.statusCode() > 0) throw ApiError(e.statusCode(), e.what());
        throw;
    }
    auto j = Json::parse(raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) throw ApiError(0, "YouTube Music answered " + endpoint + " with something that is not JSON");
    ST_LOG_DEBUG("ytmusic", "{} {} KB in {} ms", endpoint, raw.size() / 1024,
                 std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count());
    return j;
}

Json browse(const std::string& browseId, const CT& ct, const std::string& params = {}, Lang lang = Lang::Parse) {
    Json body = {{"browseId", browseId}};
    if (!params.empty()) body["params"] = params;
    return send("browse", std::move(body), ct, lang);
}

// Old-style continuations go in the URL. The tokens come percent-encoded already ("...%3D%3D"): only what is not
// URL-safe is encoded, a '%' is left as it is.
std::string continuationQuery(const std::string& token) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string t;
    for (const unsigned char c : token) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~' || c == '%') {
            t += static_cast<char>(c);
        } else {
            t += '%';
            t += kHex[c >> 4];
            t += kHex[c & 15];
        }
    }
    return "ctoken=" + t + "&continuation=" + t + "&type=next";
}

// ---- Memory cache of parsed pages --------------------------------------------------------------------------------

template <class T>
class TtlCache {
public:
    TtlCache(std::chrono::seconds ttl, size_t max) : ttl_(ttl), max_(max) {}
    std::optional<T> get(const std::string& key) {
        std::lock_guard lock(m_);
        const auto it = map_.find(key);
        if (it == map_.end()) return std::nullopt;
        if (Clock::now() - it->second->at > ttl_) {
            order_.erase(it->second);
            map_.erase(it);
            return std::nullopt;
        }
        order_.splice(order_.begin(), order_, it->second);
        ++g_hits;
        return it->second->value;
    }
    void put(const std::string& key, T value) {
        std::lock_guard lock(m_);
        if (const auto it = map_.find(key); it != map_.end()) {
            order_.erase(it->second);
            map_.erase(it);
        }
        order_.push_front({key, Clock::now(), std::move(value)});
        map_[key] = order_.begin();
        while (order_.size() > max_) {
            map_.erase(order_.back().key);
            order_.pop_back();
        }
    }
    void clear() {
        std::lock_guard lock(m_);
        map_.clear();
        order_.clear();
    }

private:
    struct Entry {
        std::string key;
        Clock::time_point at;
        T value;
    };
    std::mutex m_;
    std::chrono::seconds ttl_;
    size_t max_;
    std::list<Entry> order_;
    std::unordered_map<std::string, typename std::list<Entry>::iterator> map_;
};

TtlCache<catalog::Album>& albums() {
    static TtlCache<catalog::Album> c(30min, 48);
    return c;
}
TtlCache<ArtistResult>& artists() {
    static TtlCache<ArtistResult> c(15min, 32);
    return c;
}
TtlCache<PlaylistPage>& playlists() {
    static TtlCache<PlaylistPage> c(15min, 24);
    return c;
}
TtlCache<catalog::SearchResults>& searches() {
    static TtlCache<catalog::SearchResults> c(10min, 24);
    return c;
}

ArtistResult artistResult(const std::string& id, const CT& ct) {
    const std::string channel = rawArtist(id);
    if (channel.empty()) throw ApiError(404, "not a YouTube Music artist id: " + id);
    if (auto hit = artists().get(channel)) return std::move(*hit);
    ArtistResult r = parseArtist(browse(channel, ct), channel);
    if (r.page.artist.name.empty()) throw ApiError(404, "YouTube Music has no artist " + channel);
    artists().put(channel, r);
    return r;
}

// One radio: the first page of "next" for (videoId, playlistId), then its continuations up to `max` tracks.
std::vector<catalog::Track> watchQueue(const std::string& videoId, const std::string& playlistId, const std::string& params,
                                       size_t max, const CT& ct) {
    Json body = {{"enablePersistentPlaylistPanel", true}, {"isAudioOnly", true}, {"tunerSettingValue", "AUTOMIX_SETTING_NORMAL"}};
    if (!videoId.empty()) body["videoId"] = videoId;
    if (!playlistId.empty()) body["playlistId"] = playlistId;
    if (!params.empty()) body["params"] = params;
    WatchPage page = parseWatch(send("next", body, ct));
    std::vector<catalog::Track> out = std::move(page.tracks);
    std::unordered_set<std::string> seen;
    for (const auto& t : out) seen.insert(t.id);
    for (int i = 0; i < 4 && out.size() < max && !page.continuation.empty(); ++i) {
        try {
            page = parseWatchContinuation(send("next", body, ct, Lang::Parse, continuationQuery(page.continuation)));
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            ST_LOG_WARN("ytmusic", "radio continuation failed: {}", e.what());
            break;
        }
        size_t added = 0;
        for (auto& t : page.tracks)
            if (seen.insert(t.id).second) {
                out.push_back(std::move(t));
                ++added;
            }
        if (added == 0) break;
    }
    if (out.size() > max) out.resize(max);
    return out;
}

} // namespace

void setClient(yte::YoutubeClient c) {
    std::lock_guard lock(g_clientMutex);
    g_client.emplace(std::move(c));
}

catalog::SearchResults search(const std::string& query, int limit, const CT& ct) {
    const std::string key = std::to_string(limit) + "|" + query;
    if (auto hit = searches().get(key)) return std::move(*hit);
    auto one = [query, ct](const char* params) { return parseSearch(send("search", {{"query", query}, {"params", params}}, ct)); };
    const char* filters[] = {kSongs, kAlbums, kArtists, kFeaturedPlaylists, kCommunityPlaylists};
    std::vector<std::future<catalog::SearchResults>> parts;
    for (const char* f : filters) parts.push_back(std::async(std::launch::async, one, f));
    catalog::SearchResults out;
    std::exception_ptr firstError;
    int failed = 0;
    std::unordered_set<std::string> seenPlaylists;
    const size_t n = limit > 0 ? static_cast<size_t>(limit) : 20;
    for (size_t i = 0; i < parts.size(); ++i) {
        catalog::SearchResults r;
        try {
            r = parts[i].get();
        } catch (const yte::Exceptions::OperationCanceledException&) {
            for (size_t k = i + 1; k < parts.size(); ++k) parts[k].wait();
            throw;
        } catch (const std::exception& e) {
            ++failed;
            if (!firstError) firstError = std::current_exception();
            ST_LOG_WARN("ytmusic", "search part {} failed: {}", i, e.what());
            continue;
        }
        // Each filtered answer holds its own kind (plus, now and then, a stray row of another): take that kind only.
        if (i == 0) out.tracks = std::move(r.tracks);
        if (i == 1) out.albums = std::move(r.albums);
        if (i == 2) out.artists = std::move(r.artists);
        if (i >= 3)
            for (auto& p : r.playlists)
                if (seenPlaylists.insert(p.id).second) out.playlists.push_back(std::move(p));
    }
    if (failed == static_cast<int>(parts.size()) && firstError) std::rethrow_exception(firstError);
    if (out.tracks.size() > n) out.tracks.resize(n);
    if (out.albums.size() > n) out.albums.resize(n);
    if (out.artists.size() > n) out.artists.resize(n);
    if (out.playlists.size() > n) out.playlists.resize(n);
    if (failed == 0) searches().put(key, out);
    return out;
}

catalog::Album album(const std::string& id, const CT& ct) {
    const std::string browseId = rawAlbum(id);
    if (browseId.empty()) throw ApiError(404, "not a YouTube Music album id: " + id);
    if (auto hit = albums().get(browseId)) return std::move(*hit);
    catalog::Album a = parseAlbum(browse(browseId, ct), browseId);
    if (a.name.empty()) throw ApiError(404, "YouTube Music has no album " + browseId);
    albums().put(browseId, a);
    return a;
}

catalog::ArtistPage artistPage(const std::string& id, const CT& ct) { return artistResult(id, ct).page; }

PlaylistPage playlist(const std::string& id, const CT& ct) {
    const std::string raw = rawPlaylist(id);
    if (raw.empty()) throw ApiError(404, "not a YouTube Music playlist id: " + id);
    if (auto hit = playlists().get(raw)) return std::move(*hit);
    PlaylistPage p = parsePlaylist(browse("VL" + raw, ct), raw);
    if (p.playlist.name.empty() && p.tracks.empty()) throw ApiError(404, "YouTube Music has no playlist " + raw);
    playlists().put(raw, p);
    return p;
}

PlaylistPage playlistMore(const PlaylistPage& previous, const CT& ct) {
    if (previous.continuation.empty()) return {};
    const Json r = previous.continuationInUrl
                       ? send("browse", Json::object(), ct, Lang::Parse, continuationQuery(previous.continuation))
                       : send("browse", {{"continuation", previous.continuation}}, ct);
    PlaylistPage next = parsePlaylistContinuation(r);
    next.playlist = previous.playlist;
    return next;
}

std::vector<catalog::Track> playlistTracks(const std::string& id, size_t maxTracks, const CT& ct) {
    PlaylistPage page = playlist(id, ct);
    std::vector<catalog::Track> out = page.tracks;
    std::unordered_set<std::string> seen;
    for (const auto& t : out) seen.insert(t.id);
    for (int i = 0; i < 100 && out.size() < maxTracks && !page.continuation.empty(); ++i) {
        page = playlistMore(page, ct);
        size_t added = 0;
        for (auto& t : page.tracks)
            if (seen.insert(t.id).second) {
                out.push_back(std::move(t));
                ++added;
            }
        if (added == 0) break;
    }
    if (out.size() > maxTracks) out.resize(maxTracks);
    return out;
}

std::vector<catalog::Shelf> home(const CT& ct) {
    // Three sources in parallel: the home feed (with two continuation pages), explore, charts.
    auto feed = std::async(std::launch::async, [ct] {
        ShelvesPage page = parseShelves(browse("FEmusic_home", ct, {}, Lang::Ui));
        std::vector<catalog::Shelf> out = std::move(page.shelves);
        for (int i = 0; i < 2 && !page.continuation.empty(); ++i) {
            try {
                page = parseShelvesContinuation(send("browse", Json::object(), ct, Lang::Ui, continuationQuery(page.continuation)));
            } catch (const yte::Exceptions::OperationCanceledException&) {
                throw;
            } catch (const std::exception& e) {
                ST_LOG_WARN("ytmusic", "home continuation failed: {}", e.what());
                break;
            }
            for (auto& s : page.shelves) out.push_back(std::move(s));
        }
        return out;
    });
    auto explore = std::async(std::launch::async, [ct] { return parseShelves(browse("FEmusic_explore", ct, {}, Lang::Ui)).shelves; });
    auto charts = std::async(std::launch::async, [ct] { return parseShelves(browse("FEmusic_charts", ct, {}, Lang::Ui)).shelves; });
    std::vector<catalog::Shelf> out;
    std::exception_ptr firstError;
    int failed = 0;
    std::unordered_set<std::string> titles;
    for (auto* part : {&explore, &feed, &charts}) {
        try {
            for (auto& s : part->get())
                if (titles.insert(s.title).second) out.push_back(std::move(s));
        } catch (const yte::Exceptions::OperationCanceledException&) {
            for (auto* p : {&explore, &feed, &charts})
                if (p->valid()) p->wait();
            throw;
        } catch (const std::exception& e) {
            ++failed;
            if (!firstError) firstError = std::current_exception();
            ST_LOG_WARN("ytmusic", "home part failed: {}", e.what());
        }
    }
    if (failed == 3 && firstError) std::rethrow_exception(firstError);
    return out;
}

std::vector<Mood> moods(const CT& ct) { return parseMoods(browse("FEmusic_moods_and_genres", ct, {}, Lang::Ui)); }

std::vector<catalog::Shelf> moodShelves(const std::string& params, const CT& ct) {
    return parseShelves(browse("FEmusic_moods_and_genres_category", ct, params, Lang::Ui)).shelves;
}

std::vector<catalog::Track> radio(const std::string& seedId, size_t maxTracks, const CT& ct) {
    if (const std::string video = rawVideo(seedId); !video.empty()) return watchQueue(video, "RDAMVM" + video, {}, maxTracks, ct);
    // A collection: its own radio when YouTube Music names one, else the radio of its first song.
    std::string firstVideo;
    if (const std::string a = rawAlbum(seedId); !a.empty()) {
        const catalog::Album al = album(seedId, ct);
        if (!al.releaseId.empty()) {
            auto t = watchQueue({}, "RDAMPL" + al.releaseId, {}, maxTracks, ct);
            if (!t.empty()) return t;
        }
        if (!al.tracks.empty()) firstVideo = al.tracks.front().videoId;
    } else if (const std::string ar = rawArtist(seedId); !ar.empty()) {
        const ArtistResult r = artistResult(seedId, ct);
        if (!r.radioPlaylistId.empty()) {
            auto t = watchQueue(r.radioVideoId, r.radioPlaylistId, {}, maxTracks, ct);
            if (!t.empty()) return t;
        }
        if (!r.page.topTracks.empty()) firstVideo = r.page.topTracks.front().videoId;
    } else if (const std::string p = rawPlaylist(seedId); !p.empty()) {
        auto t = watchQueue({}, "RDAMPL" + p, {}, maxTracks, ct);
        if (!t.empty()) return t;
        const PlaylistPage page = playlist(seedId, ct);
        if (!page.tracks.empty()) firstVideo = page.tracks.front().videoId;
    }
    if (firstVideo.empty()) return {};
    return watchQueue(firstVideo, "RDAMVM" + firstVideo, {}, maxTracks, ct);
}

Stats stats() { return {g_requests.load(), g_hits.load()}; }

void clearCache() {
    albums().clear();
    artists().clear();
    playlists().clear();
    searches().clear();
}

} // namespace st::ytm
