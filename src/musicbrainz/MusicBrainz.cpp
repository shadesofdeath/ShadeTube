#include "musicbrainz/MusicBrainz.h"

#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "musicbrainz/Net.h"
#include "musicbrainz/Parse.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <format>
#include <optional>

namespace st::mb {

using detail::json;
using net::Request;
using net::Service;
using OperationCanceled = YoutubeExplode::Exceptions::OperationCanceledException;

namespace {

constexpr const char* kMb = "https://musicbrainz.org/ws/2/";
constexpr const char* kLb = "https://api.listenbrainz.org/1/";
constexpr const char* kLbLabs = "https://labs.api.listenbrainz.org/";
constexpr const char* kSimilarAlgorithm =
    "session_based_days_7500_session_300_contribution_5_threshold_10_limit_100_filter_True_skip_30";

std::atomic<bool> g_topRecordingsNeedsAuth{false};

void initTokenFromEnv() {
    static const bool once = [] {
        char* value = nullptr;
        size_t len = 0;
        if (_dupenv_s(&value, &len, "ST_LISTENBRAINZ_TOKEN") == 0 && value) {
            if (*value) net::setListenBrainzToken(value);
            free(value);
        }
        return true;
    }();
    (void)once;
}

json mbGet(const std::string& pathAndQuery, std::chrono::seconds ttl, const CT& ct) {
    Request r;
    r.service = Service::MusicBrainz;
    r.url = kMb + pathAndQuery + (pathAndQuery.find('?') == std::string::npos ? "?" : "&") + "fmt=json";
    r.ttl = ttl;
    return *net::fetch(r, ct);
}

std::optional<json> mbCached(const std::string& pathAndQuery, std::chrono::seconds ttl) {
    Request r;
    r.service = Service::MusicBrainz;
    r.url = kMb + pathAndQuery + (pathAndQuery.find('?') == std::string::npos ? "?" : "&") + "fmt=json";
    r.ttl = ttl;
    r.cacheOnly = true;
    return net::fetch(r, {});
}

json lbGet(const std::string& url, const CT& ct, bool sendToken = false) {
    initTokenFromEnv();
    Request r;
    r.service = Service::ListenBrainz;
    r.url = url;
    r.ttl = net::ttl::listenBrainz;
    r.sendToken = sendToken;
    return *net::fetch(r, ct);
}

json lbPost(const std::string& url, const json& body, const CT& ct) {
    Request r;
    r.service = Service::ListenBrainz;
    r.url = url;
    r.method = "POST";
    r.body = body.dump();
    r.ttl = net::ttl::listenBrainz;
    return *net::fetch(r, ct);
}

std::string enc(const std::string& s) { return http::urlEncode(s); }

std::string artistLookupPath(const std::string& id) { return "artist/" + id + "?inc=tags+url-rels"; }

// The Wikipedias whose article titles are kept from a Wikidata entity (the UI languages).
constexpr const char* kWikis[] = {"tr", "en", "de", "es", "fr", "pt", "ru", "uk", "id", "ja", "ko"};

// Fetches a Wikidata entity (~100 KB, never stored itself) and caches what is used of it: the P18 photo's file name
// and the article titles of kWikis.
json fetchWikidataEntity(const std::string& q, const CT& ct) {
    Request r;
    r.service = Service::Wikidata;
    r.url = "https://www.wikidata.org/wiki/Special:EntityData/" + q + ".json";
    r.ttl = net::ttl::wikidata;
    r.store = false;
    const json entity = *net::fetch(r, ct);
    net::cachePut("wikidata-p18 " + q, json{{"file", detail::wikidataImageFile(entity)}}.dump());
    json titles = json::object();
    for (const char* wiki : kWikis)
        if (std::string t = detail::wikipediaTitle(entity, std::string(wiki) + "wiki"); !t.empty()) titles[wiki] = std::move(t);
    net::cachePut("wikidata-sitelinks " + q, titles.dump());
    return entity;
}

// Commons photo of an artist lookup (url-rels): direct "image" rel, else Wikidata P18. Best effort.
std::vector<Image> artistImages(const json& lookup, const CT& ct, bool cacheOnly) {
    std::string file = detail::commonsFileFromRels(lookup);
    if (file.empty()) {
        const std::string q = detail::wikidataId(lookup);
        if (q.empty()) return {};
        const std::string key = "wikidata-p18 " + q;
        if (auto cached = net::cacheGet(key, net::ttl::wikidata)) {
            file = detail::str(json::parse(*cached, nullptr, false), "file");
        } else {
            if (cacheOnly) return {};
            try {
                file = detail::wikidataImageFile(fetchWikidataEntity(q, ct));
            } catch (const OperationCanceled&) {
                throw;
            } catch (const std::exception& e) {
                if (ct.isCancellationRequested()) throw;
                ST_LOG_WARN("mb", "wikidata {} failed: {}", q, e.what());
                return {};
            }
        }
    }
    return detail::commonsImages(file);
}

// The artist's Wikipedia lead for the About block: the article in the UI language, else the English one. Sets bio,
// bioSource, bioUrl and adds the article to the links. Best effort (throws only on cancellation).
void addWikipediaBio(ArtistPage& page, const json& lookup, const CT& ct) {
    const std::string q = detail::wikidataId(lookup);
    if (q.empty()) return;
    const std::string key = "wikidata-sitelinks " + q;
    auto cached = net::cacheGet(key, net::ttl::wikidata);
    if (!cached) {
        fetchWikidataEntity(q, ct);   // stores the titles
        cached = net::cacheGet(key, net::ttl::wikidata);
    }
    const json titles = json::parse(cached.value_or("{}"), nullptr, false);
    if (!titles.is_object()) return;
    std::vector<std::string> langs{i18n::code()};
    if (langs.front() != "en") langs.push_back("en");
    for (const auto& lang : langs) {
        const std::string title = detail::str(titles, lang);
        if (title.empty()) continue;
        std::string path = title;
        std::replace(path.begin(), path.end(), ' ', '_');
        Request r;
        r.service = Service::Wikidata;
        r.url = "https://" + lang + ".wikipedia.org/api/rest_v1/page/summary/" + enc(path);
        r.ttl = net::ttl::wikidata;
        const auto summary = detail::parseWikipediaSummary(*net::fetch(r, ct));
        if (summary.text.empty()) continue;
        page.bio = summary.text;
        page.bioSource = "Wikipedia";
        page.bioUrl = summary.url;
        if (!summary.url.empty()) page.links.push_back({"wikipedia", summary.url});
        return;
    }
}

// Fill photos only from what is already cached (no network): used for shelves of many artists.
void fillCachedImages(std::vector<Artist>& artists) {
    for (auto& a : artists) {
        if (!a.images.empty() || a.id.empty()) continue;
        try {
            if (auto lookup = mbCached(artistLookupPath(a.id), net::ttl::lookup)) {
                a.images = artistImages(*lookup, {}, true);
                if (a.tags.empty()) a.tags = detail::parseTags(*lookup);
                if (a.country.empty()) a.country = detail::str(*lookup, "country");
            }
        } catch (const std::exception&) {
        }
    }
}

std::vector<Track> topTracksFromMusicBrainz(const std::string& artistId, int limit, const CT& ct) {
    // Candidate set: the artist's official recordings (search returns releases + release groups per hit).
    std::vector<detail::RecordingHit> hits;
    const std::string q = enc("arid:" + artistId + " AND status:official AND video:false");
    constexpr int kPage = 100, kMaxPages = 3;
    for (int page = 0; page < kMaxPages; ++page) {
        const json root = mbGet(std::format("recording?query={}&limit={}&offset={}", q, kPage, page * kPage),
                                net::ttl::lookup, ct);
        auto more = detail::parseRecordingHits(root);
        const bool last = static_cast<int>(more.size()) < kPage ||
                          detail::num(root, "count") <= static_cast<int64_t>((page + 1) * kPage);
        hits.insert(hits.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        if (last) break;
    }
    // Popularity for all candidates in bulk (public, no token).
    std::unordered_map<std::string, int64_t> listens;
    try {
        constexpr size_t kChunk = 250;
        for (size_t i = 0; i < hits.size(); i += kChunk) {
            json ids = json::array();
            for (size_t k = i; k < std::min(hits.size(), i + kChunk); ++k) ids.push_back(hits[k].track.id);
            auto part = detail::parsePopularity(lbPost(std::string(kLb) + "popularity/recording",
                                                       json{{"recording_mbids", ids}}, ct));
            listens.insert(part.begin(), part.end());
        }
    } catch (const OperationCanceled&) {
        throw;
    } catch (const std::exception& e) {
        if (ct.isCancellationRequested()) throw;
        ST_LOG_WARN("mb", "popularity lookup failed: {}", e.what());
    }
    return detail::rankTopTracks(std::move(hits), listens, limit);
}

std::vector<Artist> similarByTags(const std::string& artistId, int limit, const CT& ct) {
    const Artist self = artist(artistId, ct);
    if (self.tags.empty()) return {};
    std::string q;
    for (size_t i = 0; i < std::min<size_t>(2, self.tags.size()); ++i) {
        if (!q.empty()) q += " AND ";
        q += "tag:\"" + detail::luceneEscape(self.tags[i]) + "\"";
    }
    if (!self.country.empty()) q += " AND country:" + self.country;
    q += " AND NOT arid:" + artistId;
    auto out = detail::parseArtistSearch(
        mbGet(std::format("artist?query={}&limit={}", enc(q), limit + 1), net::ttl::search, ct), limit + 1);
    std::erase_if(out, [&](const Artist& a) { return a.id == artistId; });
    if (static_cast<int>(out.size()) > limit) out.resize(static_cast<size_t>(limit));
    return out;
}

} // namespace

void setListenBrainzToken(std::string token) {
    net::setListenBrainzToken(std::move(token));
    g_topRecordingsNeedsAuth = false;
}

// ---- search --------------------------------------------------------------------------------------------------
std::vector<Artist> searchArtists(const std::string& query, int limit, const CT& ct) {
    const std::string q = detail::artistQuery(query);
    if (q.empty() || limit <= 0) return {};
    limit = std::min(limit, 100);
    return detail::parseArtistSearch(
        mbGet(std::format("artist?query={}&limit={}", enc(q), limit), net::ttl::search, ct), limit);
}

std::vector<Album> searchAlbums(const std::string& query, int limit, const CT& ct) {
    const std::string q = detail::releaseGroupQuery(query);
    if (q.empty() || limit <= 0) return {};
    const bool freeText = !detail::hasFieldPrefix(query);
    const int fetch = std::min(100, limit * 2);
    return detail::parseReleaseGroupSearch(
        mbGet(std::format("release-group?query={}&limit={}", enc(q), fetch), net::ttl::search, ct), freeText,
        limit);
}

std::vector<Track> searchTracks(const std::string& query, int limit, const CT& ct) {
    const std::string q = detail::recordingQuery(query);
    if (q.empty() || limit <= 0) return {};
    const bool freeText = !detail::hasFieldPrefix(query);
    const int fetch = std::min(100, limit * 3);
    auto hits = detail::parseRecordingHits(
        mbGet(std::format("recording?query={}&limit={}", enc(q), fetch), net::ttl::search, ct));
    return detail::rankRecordingSearch(std::move(hits), query, freeText, limit);
}

SearchResults search(const std::string& query, int limit, const CT& ct) {
    SearchResults r;
    r.artists = searchArtists(query, std::max(1, limit / 2), ct);
    r.albums = searchAlbums(query, limit, ct);
    r.tracks = searchTracks(query, limit, ct);
    return r;
}

// ---- lookups -------------------------------------------------------------------------------------------------
Artist artist(const std::string& artistId, const CT& ct) {
    const json lookup = mbGet(artistLookupPath(artistId), net::ttl::lookup, ct);
    Artist a = detail::parseArtist(lookup);
    if (a.id.empty()) a.id = artistId;
    a.images = artistImages(lookup, ct, false);
    return a;
}

std::vector<Album> artistReleaseGroups(const std::string& artistId, const CT& ct) {
    std::vector<Album> all;
    constexpr int kPage = 100, kMaxPages = 10;
    for (int page = 0; page < kMaxPages; ++page) {
        const json root = mbGet(std::format("release-group?artist={}&type=album%7Csingle%7Cep&inc=artist-credits"
                                            "&limit={}&offset={}",
                                            artistId, kPage, page * kPage),
                                net::ttl::lookup, ct);
        auto items = detail::parseReleaseGroupBrowse(root);
        const int64_t total = detail::num(root, "release-group-count");
        const bool last = static_cast<int>(items.size()) < kPage || total <= static_cast<int64_t>((page + 1) * kPage);
        all.insert(all.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
        if (last) break;
    }
    detail::sortNewestFirst(all);
    return all;
}

Album album(const std::string& releaseGroupId, const CT& ct) {
    const json rg =
        mbGet("release-group/" + releaseGroupId + "?inc=artist-credits+releases+tags+media", net::ttl::lookup, ct);
    Album a = detail::parseReleaseGroup(rg);
    if (a.id.empty()) {
        a.id = releaseGroupId;
        a.images = coverArt(a.id);
    }
    const std::string releaseId = detail::pickRepresentativeRelease(rg);
    if (releaseId.empty()) return a;
    const json release =
        mbGet("release/" + releaseId + "?inc=recordings+artist-credits+labels+media", net::ttl::lookup, ct);
    detail::applyRelease(a, release);
    return a;
}

std::string releaseGroupOfRelease(const std::string& releaseId, const CT& ct) {
    const json release = mbGet("release/" + releaseId + "?inc=release-groups", net::ttl::lookup, ct);
    return detail::str(detail::get(release, "release-group"), "id");
}

Track recording(const std::string& recordingId, const CT& ct) {
    const json rec =
        mbGet("recording/" + recordingId + "?inc=artist-credits+releases+release-groups", net::ttl::lookup, ct);
    // A lookup is one search hit without the score: the same parser picks the album.
    auto hits = detail::parseRecordingHits(json{{"recordings", json::array({rec})}});
    if (hits.empty()) throw ApiError(404, "MusicBrainz: recording " + recordingId + " not found");
    return std::move(hits.front().track);
}

Credits recordingCredits(const std::string& recordingId, const CT& ct) {
    // work-level-rels: the works' own relationships of the requested kinds (artist-rels: composer, lyricist...).
    const json rec = mbGet("recording/" + recordingId +
                               "?inc=artist-credits+artist-rels+work-rels+work-level-rels+label-rels",
                           net::ttl::lookup, ct);
    return detail::parseRecordingCredits(rec);
}

std::vector<std::string> findRecordings(const std::string& title, const std::string& artist, int durationMs, size_t max,
                                        const CT& ct) {
    const std::string t = detail::searchableTitle(title);
    if (t.empty()) return {};
    std::string q = "recording:\"" + detail::luceneEscape(t) + "\"";
    if (!artist.empty()) q += " AND artist:\"" + detail::luceneEscape(artist) + "\"";
    q += " AND video:false";
    const json root = mbGet(std::format("recording?query={}&limit=25", enc(q)), net::ttl::search, ct);
    auto ids = detail::rankRecordings(root, title, artist, durationMs);
    if (ids.size() > max) ids.resize(max);
    return ids;
}

Credits songCredits(const std::string& title, const std::string& artist, int durationMs, const CT& ct) {
    // A recording "knows" more than its artist credit when any relationship added somebody or a source.
    auto rich = [](const Credits& c) {
        if (!c.sources.empty()) return true;
        for (size_t g = 0; g < kCreditGroups; ++g)
            for (const auto& p : c.groups[g])
                if (g != static_cast<size_t>(CreditGroup::Performers) || p.roles != std::vector<std::string>{"main artist"})
                    return true;
        return false;
    };
    std::optional<Credits> first;
    for (const auto& id : findRecordings(title, artist, durationMs, 3, ct)) {
        Credits c = recordingCredits(id, ct);
        if (rich(c)) return c;
        if (!first) first = std::move(c);
    }
    return first ? std::move(*first) : Credits{"musicbrainz", {}, {}};
}

// ---- ListenBrainz --------------------------------------------------------------------------------------------
std::vector<Track> topTracksForArtist(const std::string& artistId, int limit, const CT& ct) {
    initTokenFromEnv();
    if (!g_topRecordingsNeedsAuth || net::hasListenBrainzToken()) {
        try {
            auto tracks = detail::parseLbRecordings(
                lbGet(std::string(kLb) + "popularity/top-recordings-for-artist/" + artistId, ct, true), limit);
            if (!tracks.empty()) return tracks;
        } catch (const ApiError& e) {
            if (e.status == 401 || e.status == 403) {
                if (!net::hasListenBrainzToken()) g_topRecordingsNeedsAuth = true;
            } else if (e.status != 404) {
                ST_LOG_WARN("mb", "top-recordings-for-artist: {}", e.what());
            }
        }
    }
    return topTracksFromMusicBrainz(artistId, limit, ct);
}

std::vector<Artist> similarArtists(const std::string& artistId, int limit, const CT& ct) {
    try {
        auto out = detail::parseSimilarArtists(
            lbGet(std::format("{}similar-artists/json?artist_mbids={}&algorithm={}", kLbLabs, artistId,
                              kSimilarAlgorithm),
                  ct),
            artistId, limit);
        if (!out.empty()) {
            fillCachedImages(out);
            return out;
        }
    } catch (const OperationCanceled&) {
        throw;
    } catch (const std::exception& e) {
        if (ct.isCancellationRequested()) throw;
        ST_LOG_WARN("mb", "labs similar-artists failed ({}), falling back to tag overlap", e.what());
    }
    auto out = similarByTags(artistId, limit, ct);
    fillCachedImages(out);
    return out;
}

std::vector<Artist> trendingArtists(const std::string& range, int limit, const CT& ct) {
    auto out = detail::parseLbArtists(
        lbGet(std::format("{}stats/sitewide/artists?range={}&count={}", kLb, enc(range), std::clamp(limit, 1, 100)),
              ct),
        limit);
    fillCachedImages(out);
    return out;
}

std::vector<Album> trendingAlbums(const std::string& range, int limit, const CT& ct) {
    return detail::parseLbReleaseGroups(lbGet(std::format("{}stats/sitewide/release-groups?range={}&count={}", kLb,
                                                          enc(range), std::clamp(limit * 2, 1, 100)),
                                              ct),
                                        limit);
}

std::vector<Track> trendingTracks(const std::string& range, int limit, const CT& ct) {
    return detail::parseLbRecordings(lbGet(std::format("{}stats/sitewide/recordings?range={}&count={}", kLb,
                                                       enc(range), std::clamp(limit * 2, 1, 100)),
                                           ct),
                                     limit);
}

std::vector<Album> freshReleases(int days, int limit, const CT& ct) {
    return detail::parseFreshReleases(
        lbGet(std::format("{}explore/fresh-releases/?days={}&past=true&future=false&sort=release_date", kLb,
                          std::clamp(days, 1, 90)),
              ct),
        limit);
}

// ---- composite -----------------------------------------------------------------------------------------------
ArtistPage artistPage(const std::string& artistId, const CT& ct) {
    ArtistPage page;
    page.artist = artist(artistId, ct);

    for (auto& rg : artistReleaseGroups(artistId, ct)) {
        const std::string pt = rg.primaryType;
        if (detail::isOtherRelease(rg)) page.other.push_back(std::move(rg));
        else if (pt == "Album") page.albums.push_back(std::move(rg));
        else if (pt == "Single" || pt == "EP") page.singles.push_back(std::move(rg));
        else page.other.push_back(std::move(rg));
    }

    auto optional = [&](auto&& fn, const char* what) {
        try {
            fn();
        } catch (const OperationCanceled&) {
            throw;
        } catch (const std::exception& e) {
            if (ct.isCancellationRequested()) throw;
            ST_LOG_WARN("mb", "artistPage {}: {} failed: {}", artistId, what, e.what());
        }
    };
    optional([&] { page.topTracks = topTracksForArtist(artistId, 10, ct); }, "top tracks");
    optional([&] { page.similar = similarArtists(artistId, 12, ct); }, "similar artists");
    // About: the artist's own places (url-rels of the lookup artist() just cached) and the Wikipedia lead.
    optional(
        [&] {
            const json lookup = mbGet(artistLookupPath(artistId), net::ttl::lookup, ct);
            page.links = detail::parseArtistLinks(lookup);
            addWikipediaBio(page, lookup, ct);
        },
        "about");
    return page;
}

} // namespace st::mb
