#include "ytmusic/YtMusic.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Paths.h"

#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Music/MusicClient.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>
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

std::atomic<int> g_requests{0}, g_hits{0}, g_diskHits{0};

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

// ---- Disk cache of browse answers --------------------------------------------------------------------------------
// Albums, artists, the first page of playlists, discographies and the mood tiles are kept in cache\ytmusic (pruned of
// what the parsers never read: tracking data, menus, accessibility labels; about a quarter of the answer) for a while,
// so a page opened again after a restart doesn't wait for the network. Bounded to kDiskMax (oldest files go first);
// cleared with the other caches (Ayarlar › Kitaplık ve depolama). Searches, Home, radios and continuations are not kept.

namespace disk {

constexpr uint64_t kDiskMax = 32ull << 20;
std::atomic<int> g_puts{0};

std::filesystem::path dir() { return paths::cacheDir() / L"ytmusic"; }

std::string keyOf(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

void prune(Json& j) {
    static const std::unordered_set<std::string> noise = {
        "trackingParams", "clickTrackingParams", "loggingDirectives", "accessibility", "accessibilityData",
        "frameworkUpdates", "menu", "commandMetadata", "webCommandMetadata", "serviceEndpoint", "serviceEndpoints",
        "loggingContext", "contextualMenuHeaderRenderer", "toggledAccessibilityData", "accessibilityPlayData",
        "accessibilityPauseData", "responseContext", "shareEntityEndpoint", "feedbackEndpoint"};
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end();) {
            if (noise.contains(it.key())) it = j.erase(it);
            else {
                prune(*it);
                ++it;
            }
        }
    } else if (j.is_array()) {
        for (auto& v : j) prune(v);
    }
}

// The oldest files go until the folder is under three quarters of kDiskMax.
void trim() {
    namespace fs = std::filesystem;
    std::error_code ec;
    struct File {
        fs::path path;
        fs::file_time_type at;
        uint64_t size = 0;
    };
    std::vector<File> files;
    uint64_t total = 0;
    for (const auto& e : fs::directory_iterator(dir(), ec)) {
        if (!e.is_regular_file(ec)) continue;
        File f{e.path(), e.last_write_time(ec), e.file_size(ec)};
        total += f.size;
        files.push_back(std::move(f));
    }
    if (total <= kDiskMax) return;
    std::sort(files.begin(), files.end(), [](const File& a, const File& b) { return a.at < b.at; });
    for (const auto& f : files) {
        if (total <= kDiskMax * 3 / 4) break;
        if (fs::remove(f.path, ec)) total -= std::min(total, f.size);
    }
}

std::optional<Json> get(const std::string& key, std::chrono::seconds ttl) {
    namespace fs = std::filesystem;
    const fs::path p = dir() / (key + ".json");
    std::error_code ec;
    const auto at = fs::last_write_time(p, ec);
    if (ec || fs::file_time_type::clock::now() - at > ttl) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    auto j = Json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

void put(const std::string& key, Json j) {
    namespace fs = std::filesystem;
    prune(j);
    std::error_code ec;
    fs::create_directories(dir(), ec);
    const fs::path tmp = dir() / (key + "." + std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump();
        if (!out) return;
    }
    fs::rename(tmp, dir() / (key + ".json"), ec);
    if (ec) fs::remove(tmp, ec);
    if (g_puts++ % 32 == 0) trim();
}

} // namespace disk

Json browse(const std::string& browseId, const CT& ct, const std::string& params = {}, Lang lang = Lang::Parse,
            std::chrono::seconds diskTtl = std::chrono::seconds(0)) {
    std::string key;
    if (diskTtl.count() > 0) {
        key = disk::keyOf(browseId + "|" + params + "|" + (lang == Lang::Parse ? std::string("en") : client().music().language()));
        if (auto hit = disk::get(key, diskTtl)) {
            ++g_diskHits;
            return std::move(*hit);
        }
    }
    Json body = {{"browseId", browseId}};
    if (!params.empty()) body["params"] = params;
    Json j = send("browse", std::move(body), ct, lang);
    if (!key.empty()) disk::put(key, j);
    return j;
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

// How long the disk cache keeps an answer.
constexpr std::chrono::seconds kAlbumTtl{7 * 24 * 3600}, kArtistTtl{24 * 3600}, kPlaylistTtl{6 * 3600}, kMoodsTtl{24 * 3600};

// One kind of a query, as far as it has been paged: the rows so far and where its next page starts. Playlists are the
// featured lists first, then the community lists.
struct KindList {
    catalog::SearchResults results;
    std::string continuation;
    bool started = false, done = false;
    int phase = 0;   // playlists: 0 featured, 1 community
};
TtlCache<KindList>& kindLists() {
    static TtlCache<KindList> c(10min, 48);
    return c;
}

size_t countOf(const catalog::SearchResults& r, SearchKind kind) {
    switch (kind) {
    case SearchKind::Songs: return r.tracks.size();
    case SearchKind::Albums: return r.albums.size();
    case SearchKind::Artists: return r.artists.size();
    case SearchKind::Playlists: return r.playlists.size();
    }
    return 0;
}

const char* paramsOf(SearchKind kind, int phase) {
    switch (kind) {
    case SearchKind::Songs: return kSongs;
    case SearchKind::Albums: return kAlbums;
    case SearchKind::Artists: return kArtists;
    case SearchKind::Playlists: return phase == 0 ? kFeaturedPlaylists : kCommunityPlaylists;
    }
    return kSongs;
}

// Appends the rows of `kind` in `from` not in `into` yet.
void appendKind(catalog::SearchResults& into, catalog::SearchResults& from, SearchKind kind) {
    auto merge = [](auto& to, auto& more) {
        for (auto& v : more)
            if (std::none_of(to.begin(), to.end(), [&](const auto& o) { return o.id == v.id; })) to.push_back(std::move(v));
    };
    switch (kind) {
    case SearchKind::Songs: merge(into.tracks, from.tracks); break;
    case SearchKind::Albums: merge(into.albums, from.albums); break;
    case SearchKind::Artists: merge(into.artists, from.artists); break;
    case SearchKind::Playlists: merge(into.playlists, from.playlists); break;
    }
}

template <class T>
std::vector<T> sliceOf(const std::vector<T>& v, size_t offset, size_t count) {
    if (offset >= v.size()) return {};
    return std::vector<T>(v.begin() + static_cast<std::ptrdiff_t>(offset),
                          v.begin() + static_cast<std::ptrdiff_t>(std::min(v.size(), offset + count)));
}

ArtistResult artistResult(const std::string& id, const CT& ct) {
    const std::string channel = rawArtist(id);
    if (channel.empty()) throw ApiError(404, "not a YouTube Music artist id: " + id);
    if (auto hit = artists().get(channel)) return std::move(*hit);
    ArtistResult r = parseArtist(browse(channel, ct, {}, Lang::Parse, kArtistTtl), channel);
    if (r.page.artist.name.empty()) throw ApiError(404, "YouTube Music has no artist " + channel);
    // The page shows about ten albums and ten singles; their "More" pages hold all of them (artistReleases, asked when
    // the discography grid opens). The size of a group isn't given: one more than shown says "there may be more".
    if (!r.allAlbums.empty() && !r.page.albums.empty()) r.page.albumsTotal = static_cast<int>(r.page.albums.size()) + 1;
    if (!r.allSingles.empty() && !r.page.singles.empty()) r.page.singlesTotal = static_cast<int>(r.page.singles.size()) + 1;
    // The page shows five top songs without their length; "Top songs" opens the artist's songs by popularity, the
    // same five first, with lengths and albums: the first 20 of it are the top songs.
    if (!r.songsPlaylistId.empty()) {
        try {
            const PlaylistPage songs = parsePlaylist(browse("VL" + r.songsPlaylistId, ct, {}, Lang::Parse, kArtistTtl), r.songsPlaylistId);
            if (songs.tracks.size() >= r.page.topTracks.size()) {
                r.page.topTracks.assign(songs.tracks.begin(), songs.tracks.begin() + std::min<size_t>(20, songs.tracks.size()));
                for (auto& t : r.page.topTracks)
                    if (t.artists.empty()) t.artists = {{r.page.artist.id, r.page.artist.name}};
            }
        } catch (const yte::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            ST_LOG_WARN("ytmusic", "songs of {} not loaded: {}", channel, e.what());
        }
    }
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

namespace {
void trimTo(catalog::SearchResults& r, size_t n) {
    if (r.tracks.size() > n) r.tracks.resize(n);
    if (r.albums.size() > n) r.albums.resize(n);
    if (r.artists.size() > n) r.artists.resize(n);
    if (r.playlists.size() > n) r.playlists.resize(n);
}
} // namespace

catalog::SearchResults searchQuick(const std::string& query, int limit, const CT& ct) {
    const std::string key = "quick|" + std::to_string(limit) + "|" + query;
    if (auto hit = searches().get(key)) return std::move(*hit);
    catalog::SearchResults r = parseSearch(send("search", {{"query", query}}, ct));
    trimTo(r, limit > 0 ? static_cast<size_t>(limit) : 20);
    searches().put(key, r);
    return r;
}

SearchBatch searchMore(const std::string& query, SearchKind kind, int offset, int count, const CT& ct) {
    const std::string key = std::to_string(static_cast<int>(kind)) + "|" + query;
    KindList l = kindLists().get(key).value_or(KindList{});
    const size_t want = static_cast<size_t>(std::max(0, offset)) + static_cast<size_t>(std::max(1, count));
    for (int guard = 0; guard < 12 && countOf(l.results, kind) < want && !l.done; ++guard) {
        Json r;
        if (!l.started) {
            r = send("search", {{"query", query}, {"params", paramsOf(kind, l.phase)}}, ct);
            l.started = true;
        } else if (!l.continuation.empty()) {
            r = send("search", Json::object(), ct, Lang::Parse, continuationQuery(l.continuation));
        } else if (kind == SearchKind::Playlists && l.phase == 0) {   // the featured lists ran out: the community's
            l.phase = 1;
            l.started = false;
            continue;
        } else {
            l.done = true;
            break;
        }
        SearchPage page = parseSearchPage(r);
        const size_t before = countOf(l.results, kind);
        appendKind(l.results, page.results, kind);
        l.continuation = page.continuation;
        if (countOf(l.results, kind) == before && l.continuation.empty() && !(kind == SearchKind::Playlists && l.phase == 0))
            l.done = true;
    }
    if (l.continuation.empty() && l.started && !(kind == SearchKind::Playlists && l.phase == 0)) l.done = true;
    kindLists().put(key, l);
    SearchBatch b;
    const size_t from = static_cast<size_t>(std::max(0, offset)), n = static_cast<size_t>(std::max(1, count));
    b.results.tracks = sliceOf(l.results.tracks, from, n);
    b.results.albums = sliceOf(l.results.albums, from, n);
    b.results.artists = sliceOf(l.results.artists, from, n);
    b.results.playlists = sliceOf(l.results.playlists, from, n);
    b.hasMore = !l.done || countOf(l.results, kind) > from + n;
    return b;
}

catalog::SearchResults search(const std::string& query, int limit, const CT& ct) {
    const std::string key = std::to_string(limit) + "|" + query;
    if (auto hit = searches().get(key)) return std::move(*hit);
    // The unfiltered answer (the top result) and the first page of each kind, in parallel.
    const int n = limit > 0 ? limit : 20;
    auto top = std::async(std::launch::async, [query, ct] { return parseSearch(send("search", {{"query", query}}, ct)); });
    const SearchKind kinds[] = {SearchKind::Songs, SearchKind::Albums, SearchKind::Artists, SearchKind::Playlists};
    std::vector<std::future<SearchBatch>> parts;
    for (const SearchKind k : kinds)
        parts.push_back(std::async(std::launch::async, [query, k, n, ct] { return searchMore(query, k, 0, n, ct); }));
    catalog::SearchResults out, overview;
    std::exception_ptr firstError;
    int failed = 0;
    bool kindFailed[4] = {};
    for (size_t i = 0; i < parts.size(); ++i) {
        try {
            SearchBatch b = parts[i].get();
            appendKind(out, b.results, kinds[i]);
        } catch (const yte::Exceptions::OperationCanceledException&) {
            for (size_t k = i + 1; k < parts.size(); ++k) parts[k].wait();
            top.wait();
            throw;
        } catch (const std::exception& e) {
            ++failed;
            kindFailed[i] = true;
            if (!firstError) firstError = std::current_exception();
            ST_LOG_WARN("ytmusic", "search of kind {} failed: {}", i, e.what());
        }
    }
    try {
        overview = top.get();
    } catch (const yte::Exceptions::OperationCanceledException&) {
        throw;
    } catch (const std::exception& e) {
        ++failed;
        if (!firstError) firstError = std::current_exception();
        ST_LOG_WARN("ytmusic", "unfiltered search failed: {}", e.what());
    }
    if (failed == 5 && firstError) std::rethrow_exception(firstError);
    // A kind whose own search failed: what the overview has of it.
    for (size_t i = 0; i < 4; ++i)
        if (kindFailed[i]) {
            catalog::SearchResults copy = overview;
            appendKind(out, copy, kinds[i]);
        }
    // The top result is a row of one of the lists: put it in front of its list when that list doesn't have it.
    out.topResult = overview.topResult;
    if (!out.topResult.empty()) {
        auto front = [&](auto& list, auto& from) {
            const auto it = std::find_if(from.begin(), from.end(), [&](const auto& v) { return v.id == out.topResult; });
            if (it == from.end()) return false;
            if (std::none_of(list.begin(), list.end(), [&](const auto& v) { return v.id == out.topResult; }))
                list.insert(list.begin(), *it);
            return true;
        };
        if (!front(out.artists, overview.artists) && !front(out.albums, overview.albums) &&
            !front(out.tracks, overview.tracks) && !front(out.playlists, overview.playlists))
            out.topResult.clear();
    }
    if (failed == 0) searches().put(key, out);
    return out;
}

catalog::SearchResults searchKind(const std::string& query, SearchKind kind, const CT& ct) {
    return parseSearch(send("search", {{"query", query}, {"params", paramsOf(kind, 0)}}, ct));
}

std::vector<catalog::Album> artistReleases(const std::string& id, catalog::ReleaseGroup group, const CT& ct) {
    const ArtistResult a = artistResult(id, ct);
    const BrowseRef& ref = group == catalog::ReleaseGroup::Albums    ? a.allAlbums
                           : group == catalog::ReleaseGroup::Singles ? a.allSingles
                                                                     : BrowseRef{};
    if (ref.empty()) return {};
    const catalog::ArtistRef owner{a.page.artist.id, a.page.artist.name};
    ReleasesPage page = parseReleases(browse(ref.id, ct, ref.params, Lang::Parse, kArtistTtl), owner);
    std::vector<catalog::Album> out = std::move(page.releases);
    for (int i = 0; i < 10 && !page.continuation.empty(); ++i) {
        page = parseReleases(send("browse", Json::object(), ct, Lang::Parse, continuationQuery(page.continuation)), owner);
        const size_t before = out.size();
        for (auto& r : page.releases)
            if (std::none_of(out.begin(), out.end(), [&](const catalog::Album& o) { return o.id == r.id; })) out.push_back(std::move(r));
        if (out.size() == before) break;
    }
    if (group == catalog::ReleaseGroup::Singles)
        for (auto& r : out)
            if (r.primaryType == "Album") r.primaryType = "Single";
    return out;
}

catalog::Album album(const std::string& id, const CT& ct) {
    const std::string browseId = rawAlbum(id);
    if (browseId.empty()) throw ApiError(404, "not a YouTube Music album id: " + id);
    if (auto hit = albums().get(browseId)) return std::move(*hit);
    catalog::Album a = parseAlbum(browse(browseId, ct, {}, Lang::Parse, kAlbumTtl), browseId);
    if (a.name.empty()) throw ApiError(404, "YouTube Music has no album " + browseId);
    albums().put(browseId, a);
    return a;
}

catalog::ArtistPage artistPage(const std::string& id, const CT& ct) { return artistResult(id, ct).page; }

PlaylistPage playlist(const std::string& id, const CT& ct) {
    const std::string raw = rawPlaylist(id);
    if (raw.empty()) throw ApiError(404, "not a YouTube Music playlist id: " + id);
    if (auto hit = playlists().get(raw)) return std::move(*hit);
    PlaylistPage p = parsePlaylist(browse("VL" + raw, ct, {}, Lang::Parse, kPlaylistTtl), raw);
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
        // The feed's continuations are answered for the visitor it was made for: MusicClient keeps one per session.
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

std::vector<Mood> moods(const CT& ct) { return parseMoods(browse("FEmusic_moods_and_genres", ct, {}, Lang::Ui, kMoodsTtl)); }

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

Stats stats() { return {g_requests.load(), g_hits.load(), g_diskHits.load()}; }

void clearCache() {
    albums().clear();
    artists().clear();
    playlists().clear();
    searches().clear();
    kindLists().clear();
}

void clearDiskCache() {
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(disk::dir(), ec)) std::filesystem::remove(e.path(), ec);
}

uint64_t diskCacheBytes() {
    std::error_code ec;
    uint64_t total = 0;
    for (const auto& e : std::filesystem::directory_iterator(disk::dir(), ec))
        if (e.is_regular_file(ec)) total += e.file_size(ec);
    return total;
}

} // namespace st::ytm
