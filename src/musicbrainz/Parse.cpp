#include "musicbrainz/Parse.h"

#include "core/Http.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <unordered_set>

namespace st::mb::detail {

namespace {

const json& nullJson() {
    static const json n = nullptr;
    return n;
}
const json& emptyArray() {
    static const json a = json::array();
    return a;
}

std::string asciiLower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return std::string(s.substr(b, e - b));
}

bool containsWord(const std::string& haystackLower, std::string_view needle) {
    return haystackLower.find(needle) != std::string::npos;
}

// Versions that usually aren't what someone means when they search for / play "the song".
bool isAlternateVersion(const std::string& titleLower) {
    static constexpr std::array<std::string_view, 12> kMarkers = {
        "remix", " mix)", " mix]", "karaoke", "instrumental", "live", "acoustic", "akustik",
        "cover", "version)", "edit)", "remaster"};
    for (auto m : kMarkers)
        if (containsWord(titleLower, m)) return true;
    return false;
}

std::string percentDecode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

std::vector<std::string> strArray(const json& a) {
    std::vector<std::string> out;
    if (!a.is_array()) return out;
    for (const auto& v : a)
        if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

bool isExcludedSecondary(const std::string& t) {
    const std::string l = asciiLower(t);
    return l == "compilation" || l == "live" || l == "remix" || l == "dj-mix" || l == "demo";
}

// LB rows credit artists as {artist_mbids: [...], artist_name: "A feat. B"}.
std::vector<ArtistRef> lbArtists(const json& row, std::string_view nameKey) {
    std::vector<ArtistRef> out;
    const auto mbids = strArray(get(row, "artist_mbids"));
    std::string name = str(row, nameKey);
    if (name.empty()) name = str(row, "artist_name");
    std::string id = mbids.empty() ? str(row, "artist_mbid") : mbids.front();
    if (!name.empty() || !id.empty()) out.push_back({std::move(id), std::move(name)});
    return out;
}

const json& lbPayloadList(const json& root, std::string_view key) {
    if (root.is_array()) return root;
    const json& p = get(root, "payload");
    const json& inner = get(p.is_null() ? root : p, key);
    return inner.is_array() ? inner : emptyArray();
}

int primaryRank(const json& rg) {
    const std::string pt = asciiLower(str(rg, "primary-type"));
    const bool clean = arr(rg, "secondary-types").empty();
    if (pt == "album" && clean) return 0;
    if ((pt == "single" || pt == "ep") && clean) return 1;
    if (pt == "album") return 2;
    return 3;
}

} // namespace

// ---- accessors -----------------------------------------------------------------------------------------------
const json& get(const json& j, std::string_view key) {
    if (!j.is_object()) return nullJson();
    auto it = j.find(key);
    return it == j.end() ? nullJson() : *it;
}

std::string str(const json& j, std::string_view key) {
    const json& v = get(j, key);
    return v.is_string() ? v.get<std::string>() : std::string{};
}

int64_t num(const json& j, std::string_view key, int64_t def) {
    const json& v = get(j, key);
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number()) return static_cast<int64_t>(v.get<double>());
    if (v.is_string()) {
        try {
            return std::stoll(v.get<std::string>());
        } catch (...) {
        }
    }
    return def;
}

const json& arr(const json& j, std::string_view key) {
    const json& v = get(j, key);
    return v.is_array() ? v : emptyArray();
}

// ---- queries -------------------------------------------------------------------------------------------------
bool hasFieldPrefix(std::string_view q) {
    static const std::unordered_set<std::string> kFields = {
        "tag", "artist", "artistname", "arid", "alias", "country", "releasegroup", "release", "recording",
        "rgid", "reid", "rid", "primarytype", "secondarytype", "type", "status", "date", "firstreleasedate",
        "creditname", "isrc", "dur", "qdur", "tnum", "tracks", "gender", "area", "begin", "end", "label",
        "format", "comment", "sortname", "ipi", "isni", "lang", "script", "video", "position", "barcode",
        "catno", "tid", "mbid", "releaseaccent", "recordingaccent", "artistaccent", "releasegroupaccent"};
    for (size_t i = 0; i < q.size(); ++i) {
        const bool boundary = i == 0 || std::isspace(static_cast<unsigned char>(q[i - 1])) || q[i - 1] == '(' ||
                              q[i - 1] == '+' || q[i - 1] == '-' || q[i - 1] == '!';
        if (!boundary || !std::isalpha(static_cast<unsigned char>(q[i]))) continue;
        size_t j = i;
        while (j < q.size() && (std::isalpha(static_cast<unsigned char>(q[j])) || q[j] == '-' || q[j] == '_')) ++j;
        if (j < q.size() && q[j] == ':' && kFields.contains(asciiLower(std::string(q.substr(i, j - i))))) return true;
    }
    return false;
}

std::string luceneEscape(std::string_view text) {
    static constexpr std::string_view kSpecial = "+-&|!(){}[]^\"~*?:\\/";
    std::string out;
    out.reserve(text.size() + 8);
    for (char c : text) {
        if (kSpecial.find(c) != std::string_view::npos) out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

static std::string prepared(const std::string& q) {
    // Lowercase ASCII so user-typed AND/OR/NOT are plain words, then escape.
    return luceneEscape(asciiLower(trim(q)));
}

std::string artistQuery(const std::string& q) {
    if (hasFieldPrefix(q)) return q;
    const std::string e = prepared(q);
    if (e.empty()) return {};
    return "artist:\"" + e + "\"^3 OR artist:(" + e + ") OR alias:(" + e + ")";
}

std::string releaseGroupQuery(const std::string& q) {
    if (hasFieldPrefix(q)) return q;
    const std::string e = prepared(q);
    if (e.empty()) return {};
    return "(releasegroup:(" + e + ") OR artist:(" + e +
           ")) AND status:official AND (primarytype:album OR primarytype:ep OR primarytype:single)";
}

std::string recordingQuery(const std::string& q) {
    if (hasFieldPrefix(q)) return q;
    const std::string e = prepared(q);
    if (e.empty()) return {};
    return "(recording:(" + e + ") OR artist:(" + e + ")) AND video:false";
}

// ---- MusicBrainz entities ------------------------------------------------------------------------------------
std::vector<ArtistRef> parseArtistCredit(const json& credit) {
    std::vector<ArtistRef> out;
    if (!credit.is_array()) return out;
    for (const auto& c : credit) {
        const json& a = get(c, "artist");
        ArtistRef ref{str(a, "id"), str(c, "name")};
        if (ref.name.empty()) ref.name = str(a, "name");
        if (!ref.name.empty() || !ref.id.empty()) out.push_back(std::move(ref));
    }
    return out;
}

std::vector<std::string> parseTags(const json& entity, size_t max) {
    std::vector<std::pair<int64_t, std::string>> tags;
    for (const auto& t : arr(entity, "tags")) {
        std::string name = str(t, "name");
        const int64_t count = num(t, "count");
        if (!name.empty() && count >= 0) tags.emplace_back(count, std::move(name));
    }
    std::stable_sort(tags.begin(), tags.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<std::string> out;
    for (auto& [c, n] : tags) {
        if (out.size() >= max) break;
        out.push_back(std::move(n));
    }
    return out;
}

Artist parseArtist(const json& a) {
    Artist r;
    r.id = str(a, "id");
    r.name = str(a, "name");
    r.type = str(a, "type");
    r.country = str(a, "country");
    if (r.country.empty()) {
        const auto codes = strArray(get(get(a, "area"), "iso-3166-1-codes"));
        if (!codes.empty()) r.country = codes.front();
    }
    r.disambiguation = str(a, "disambiguation");
    const std::string begin = str(get(a, "life-span"), "begin");
    r.beginYear = begin.substr(0, std::min<size_t>(4, begin.size()));
    r.tags = parseTags(a);
    return r;
}

std::vector<Artist> parseArtistSearch(const json& root, int limit) {
    std::vector<Artist> out;
    for (const auto& a : arr(root, "artists")) {
        if (static_cast<int>(out.size()) >= limit) break;
        Artist artist = parseArtist(a);
        if (!artist.id.empty() && !artist.name.empty()) out.push_back(std::move(artist));
    }
    return out;
}

std::string wikidataId(const json& artistLookup) {
    for (const auto& rel : arr(artistLookup, "relations")) {
        if (asciiLower(str(rel, "type")) != "wikidata") continue;
        const std::string url = str(get(rel, "url"), "resource");
        const auto slash = url.find_last_of('/');
        std::string id = slash == std::string::npos ? url : url.substr(slash + 1);
        if (id.size() > 1 && (id[0] == 'Q' || id[0] == 'q')) return id;
    }
    return {};
}

std::string commonsFileFromRels(const json& artistLookup) {
    for (const auto& rel : arr(artistLookup, "relations")) {
        if (asciiLower(str(rel, "type")) != "image") continue;
        const std::string url = str(get(rel, "url"), "resource");
        if (url.find("commons.wikimedia.org") == std::string::npos) continue;
        const auto pos = url.find("File:");
        if (pos != std::string::npos) return percentDecode(url.substr(pos + 5));
    }
    return {};
}

std::string wikidataImageFile(const json& entityData) {
    const json& entities = get(entityData, "entities");
    if (!entities.is_object() || entities.empty()) return {};
    const json& claims = arr(get(entities.begin().value(), "claims"), "P18");
    std::string normal;
    for (const auto& c : claims) {
        const std::string rank = str(c, "rank");
        if (rank == "deprecated") continue;
        const json& value = get(get(get(c, "mainsnak"), "datavalue"), "value");
        if (!value.is_string()) continue;
        if (rank == "preferred") return value.get<std::string>();
        if (normal.empty()) normal = value.get<std::string>();
    }
    return normal;
}

std::vector<Image> commonsImages(const std::string& fileName) {
    if (fileName.empty()) return {};
    std::string f = fileName;
    std::replace(f.begin(), f.end(), ' ', '_');
    const std::string base = "https://commons.wikimedia.org/wiki/Special:FilePath/" + http::urlEncode(f) + "?width=";
    return {{base + "250", 250, 0}, {base + "500", 500, 0}, {base + "1000", 1000, 0}};
}

Album parseReleaseGroup(const json& rg) {
    Album a;
    a.id = str(rg, "id");
    a.name = str(rg, "title");
    a.primaryType = str(rg, "primary-type");
    a.secondaryTypes = strArray(get(rg, "secondary-types"));
    a.artists = parseArtistCredit(get(rg, "artist-credit"));
    a.firstReleaseDate = str(rg, "first-release-date");
    a.images = coverArt(a.id);
    return a;
}

std::vector<Album> parseReleaseGroupSearch(const json& root, bool freeText, int limit) {
    struct Ranked {
        Album album;
        double rank;
    };
    std::vector<Ranked> ranked;
    for (const auto& rg : arr(root, "release-groups")) {
        Album a = parseReleaseGroup(rg);
        if (a.id.empty()) continue;
        const std::string pt = asciiLower(a.primaryType);
        if (freeText && pt != "album" && pt != "ep" && pt != "single") continue;
        double rank = static_cast<double>(num(rg, "score"));
        rank += 6.0 * std::log2(1.0 + static_cast<double>(num(rg, "count")));
        if (!a.secondaryTypes.empty()) rank -= 8.0;
        ranked.push_back({std::move(a), rank});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& x, const Ranked& y) { return x.rank > y.rank; });
    std::vector<Album> out;
    for (auto& r : ranked) {
        if (static_cast<int>(out.size()) >= limit) break;
        out.push_back(std::move(r.album));
    }
    return out;
}

std::vector<Album> parseReleaseGroupBrowse(const json& root) {
    std::vector<Album> out;
    for (const auto& rg : arr(root, "release-groups")) {
        Album a = parseReleaseGroup(rg);
        if (!a.id.empty()) out.push_back(std::move(a));
    }
    return out;
}

bool isOtherRelease(const Album& a) {
    for (const auto& t : a.secondaryTypes)
        if (isExcludedSecondary(t)) return true;
    return false;
}

void sortNewestFirst(std::vector<Album>& albums) {
    std::stable_sort(albums.begin(), albums.end(), [](const Album& x, const Album& y) {
        if (x.firstReleaseDate.empty() != y.firstReleaseDate.empty()) return y.firstReleaseDate.empty();
        return x.firstReleaseDate > y.firstReleaseDate;   // ISO dates compare lexicographically
    });
}

// ---- recordings ----------------------------------------------------------------------------------------------
std::vector<RecordingHit> parseRecordingHits(const json& root) {
    std::vector<RecordingHit> out;
    for (const auto& r : arr(root, "recordings")) {
        RecordingHit h;
        h.track.id = str(r, "id");
        h.track.name = str(r, "title");
        if (h.track.id.empty() || h.track.name.empty()) continue;
        h.track.durationMs = static_cast<int>(num(r, "length"));
        h.track.artists = parseArtistCredit(get(r, "artist-credit"));
        h.score = static_cast<double>(num(r, "score"));

        const json& releases = arr(r, "releases");
        h.releaseCount = static_cast<int>(releases.size());
        const json* best = nullptr;
        int bestRank = 1 << 20;
        for (const auto& rel : releases) {
            const bool official = asciiLower(str(rel, "status")) == "official";
            h.official = h.official || official;
            const json& rg = get(rel, "release-group");
            if (str(rg, "id").empty()) continue;
            const int rank = primaryRank(rg) + (official ? 0 : 10);
            if (rank < bestRank) {
                bestRank = rank;
                best = &rg;
            }
        }
        if (best) {
            h.track.album.id = str(*best, "id");
            h.track.album.name = str(*best, "title");
            h.track.album.images = coverArt(h.track.album.id);
        }
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<Track> rankRecordingSearch(std::vector<RecordingHit> hits, const std::string& query, bool freeText,
                                       int limit) {
    // Official-only when that leaves anything.
    if (std::any_of(hits.begin(), hits.end(), [](const RecordingHit& h) { return h.official; }))
        std::erase_if(hits, [](const RecordingHit& h) { return !h.official; });

    const std::string q = asciiLower(trim(query));
    struct Ranked {
        RecordingHit* hit;
        double rank;
        std::string key;
    };
    std::vector<Ranked> ranked;
    for (auto& h : hits) {
        const std::string titleLower = asciiLower(h.track.name);
        const std::string artistLower = asciiLower(h.track.artistLine());
        double rank = h.score + 8.0 * std::log2(1.0 + h.releaseCount);
        if (freeText) {
            for (const auto& a : h.track.artists)
                if (asciiLower(a.name) == q) rank += 25.0;
            if (isAlternateVersion(titleLower) && !isAlternateVersion(q + ")")) rank -= 15.0;
        }
        if (h.track.durationMs <= 0) rank -= 5.0;
        ranked.push_back({&h, rank, titleLower + "\x1f" + artistLower});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) { return a.rank > b.rank; });

    std::vector<Track> out;
    std::unordered_map<std::string, size_t> seen;
    for (auto& r : ranked) {
        if (auto it = seen.find(r.key); it != seen.end()) {
            Track& kept = out[it->second];
            if (kept.durationMs <= 0 && r.hit->track.durationMs > 0) kept.durationMs = r.hit->track.durationMs;
            if (kept.album.id.empty() && !r.hit->track.album.id.empty()) kept.album = r.hit->track.album;
            continue;
        }
        if (static_cast<int>(out.size()) >= limit) continue;   // keep scanning to fill gaps of kept rows
        seen.emplace(r.key, out.size());
        out.push_back(std::move(r.hit->track));
    }
    return out;
}

std::unordered_map<std::string, int64_t> parsePopularity(const json& root) {
    std::unordered_map<std::string, int64_t> out;
    const json& list = root.is_array() ? root : arr(root, "payload");
    for (const auto& row : list) {
        const std::string id = str(row, "recording_mbid");
        if (!id.empty()) out[id] = std::max<int64_t>(0, num(row, "total_listen_count"));
    }
    return out;
}

std::vector<Track> rankTopTracks(std::vector<RecordingHit> hits, const std::unordered_map<std::string, int64_t>& listens,
                                 int limit) {
    if (std::any_of(hits.begin(), hits.end(), [](const RecordingHit& h) { return h.official; }))
        std::erase_if(hits, [](const RecordingHit& h) { return !h.official; });

    struct Group {
        RecordingHit* best = nullptr;
        int64_t bestListens = -1;
        int64_t totalListens = 0;
        int releases = 0;
        bool alternate = false;
    };
    std::unordered_map<std::string, Group> groups;
    std::vector<std::string> order;
    for (auto& h : hits) {
        const std::string key = asciiLower(trim(h.track.name));
        const auto it = listens.find(h.track.id);
        const int64_t l = it == listens.end() ? 0 : it->second;
        auto [git, inserted] = groups.try_emplace(key);
        if (inserted) order.push_back(key);
        Group& g = git->second;
        g.totalListens += l;
        g.releases += h.releaseCount;
        const bool better = !g.best || l > g.bestListens ||
                            (l == g.bestListens && (h.releaseCount > g.best->releaseCount ||
                                                    (g.best->track.album.id.empty() && !h.track.album.id.empty())));
        if (better) {
            g.best = &h;
            g.bestListens = l;
        }
        g.alternate = isAlternateVersion(key);
    }
    std::vector<Group*> sorted;
    for (const auto& k : order) sorted.push_back(&groups[k]);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Group* a, const Group* b) {
        if (a->totalListens != b->totalListens) return a->totalListens > b->totalListens;
        if (a->alternate != b->alternate) return !a->alternate;
        return a->releases > b->releases;
    });
    std::vector<Track> out;
    for (Group* g : sorted) {
        if (static_cast<int>(out.size()) >= limit) break;
        Track t = g->best->track;
        t.listenCount = g->totalListens;
        out.push_back(std::move(t));
    }
    return out;
}

// ---- releases ------------------------------------------------------------------------------------------------
std::string pickRepresentativeRelease(const json& rgLookup) {
    const json& releases = arr(rgLookup, "releases");
    if (releases.empty()) return {};
    const bool anyOfficial = std::any_of(releases.begin(), releases.end(),
                                         [](const json& r) { return asciiLower(str(r, "status")) == "official"; });

    auto trackCount = [](const json& r) {
        int64_t n = 0;
        for (const auto& m : arr(r, "media")) n += num(m, "track-count");
        return n;
    };
    int64_t minTracks = -1;
    for (const auto& r : releases) {
        if (anyOfficial && asciiLower(str(r, "status")) != "official") continue;
        const int64_t n = trackCount(r);
        if (n > 0 && (minTracks < 0 || n < minTracks)) minTracks = n;
    }

    const json* best = nullptr;
    double bestScore = -1e18;
    std::string bestDate;
    for (const auto& r : releases) {
        if (str(r, "id").empty()) continue;
        if (anyOfficial && asciiLower(str(r, "status")) != "official") continue;
        double score = 0;
        const std::string country = str(r, "country");
        if (country == "XW") score += 20;
        else if (country == "TR") score += 15;
        int good = 0, bad = 0;
        for (const auto& m : arr(r, "media")) {
            const std::string f = asciiLower(str(m, "format"));
            if (f.find("cd") != std::string::npos || f.find("digital") != std::string::npos) ++good;
            else if (!f.empty()) ++bad;
        }
        if (good && !bad) score += 10;
        else if (bad && !good) score -= 5;
        const int64_t n = trackCount(r);
        if (minTracks > 0 && n > minTracks) score -= 2.0 * static_cast<double>(n - minTracks);
        if (n == 0) score -= 3;
        const std::string date = str(r, "date");
        const bool earlier = !date.empty() && (bestDate.empty() || date < bestDate);
        if (!best || score > bestScore || (score == bestScore && earlier)) {
            best = &r;
            bestScore = score;
            bestDate = date;
        }
    }
    return best ? str(*best, "id") : std::string{};
}

void applyRelease(Album& album, const json& release) {
    album.releaseId = str(release, "id");
    if (album.artists.empty()) album.artists = parseArtistCredit(get(release, "artist-credit"));
    if (album.firstReleaseDate.empty()) album.firstReleaseDate = str(release, "date");
    for (const auto& li : arr(release, "label-info")) {
        const std::string name = str(get(li, "label"), "name");
        if (!name.empty() && name != "[no label]") {
            album.label = name;
            break;
        }
    }
    AlbumRef ref{album.id, album.name, album.images};
    album.tracks.clear();
    for (const auto& medium : arr(release, "media")) {
        const json& tracks = get(medium, "tracks").is_array() ? arr(medium, "tracks") : arr(medium, "track");
        for (const auto& t : tracks) {
            const json& rec = get(t, "recording");
            Track tr;
            tr.id = str(rec, "id");
            if (tr.id.empty()) tr.id = str(t, "id");
            tr.name = str(t, "title");
            if (tr.name.empty()) tr.name = str(rec, "title");
            int64_t len = num(t, "length");
            if (len <= 0) len = num(rec, "length");
            tr.durationMs = static_cast<int>(len);
            tr.trackNumber = static_cast<int>(num(t, "position"));
            tr.artists = parseArtistCredit(get(t, "artist-credit"));
            if (tr.artists.empty()) tr.artists = parseArtistCredit(get(rec, "artist-credit"));
            if (tr.artists.empty()) tr.artists = album.artists;
            tr.album = ref;
            if (!tr.id.empty()) album.tracks.push_back(std::move(tr));
        }
    }
    album.totalTracks = static_cast<int>(album.tracks.size());
}

// ---- ListenBrainz --------------------------------------------------------------------------------------------
std::vector<Image> caaReleaseImages(const std::string& releaseMbid, int64_t caaId) {
    if (releaseMbid.empty()) return {};
    const std::string base = "https://coverartarchive.org/release/" + releaseMbid + "/";
    if (caaId > 0) {
        const std::string id = std::to_string(caaId);
        return {{base + id + "-250.jpg", 250, 250}, {base + id + "-500.jpg", 500, 500},
                {base + id + "-1200.jpg", 1200, 1200}};
    }
    return {{base + "front-250", 250, 250}, {base + "front-500", 500, 500}, {base + "front-1200", 1200, 1200}};
}

std::vector<Artist> parseLbArtists(const json& root, int limit) {
    std::vector<Artist> out;
    std::unordered_set<std::string> seen;
    for (const auto& row : lbPayloadList(root, "artists")) {
        if (static_cast<int>(out.size()) >= limit) break;
        Artist a;
        a.id = str(row, "artist_mbid");
        if (a.id.empty()) {
            const auto ids = strArray(get(row, "artist_mbids"));
            if (!ids.empty()) a.id = ids.front();
        }
        a.name = str(row, "artist_name");
        a.listenCount = num(row, "listen_count");
        if (a.id.empty() || a.name.empty() || !seen.insert(a.id).second) continue;
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<Album> parseLbReleaseGroups(const json& root, int limit) {
    std::vector<Album> out;
    std::unordered_set<std::string> seen;
    for (const auto& row : lbPayloadList(root, "release_groups")) {
        if (static_cast<int>(out.size()) >= limit) break;
        Album a;
        a.id = str(row, "release_group_mbid");
        if (a.id.empty() || !seen.insert(a.id).second) continue;
        a.name = str(row, "release_group_name");
        a.releaseId = str(row, "caa_release_mbid");
        a.artists = lbArtists(row, "artist_name");
        a.images = coverArt(a.id);
        out.push_back(std::move(a));
    }
    return out;
}

std::vector<Track> parseLbRecordings(const json& root, int limit) {
    std::vector<Track> out;
    std::unordered_set<std::string> seen;
    for (const auto& row : lbPayloadList(root, "recordings")) {
        if (static_cast<int>(out.size()) >= limit) break;
        Track t;
        t.id = str(row, "recording_mbid");
        if (t.id.empty() || !seen.insert(t.id).second) continue;
        t.name = str(row, "track_name");
        if (t.name.empty()) t.name = str(row, "recording_name");
        t.artists = lbArtists(row, "artist_name");
        t.durationMs = static_cast<int>(num(row, "length"));
        t.listenCount = num(row, "listen_count", num(row, "total_listen_count"));
        t.album.id = str(row, "release_group_mbid");
        t.album.name = str(row, "release_group_name");
        if (t.album.name.empty()) t.album.name = str(row, "release_name");
        if (!t.album.id.empty()) t.album.images = coverArt(t.album.id);
        else t.album.images = caaReleaseImages(str(row, "caa_release_mbid"), num(row, "caa_id"));
        if (t.album.images.empty() && !str(row, "release_mbid").empty() && num(row, "caa_id") > 0)
            t.album.images = caaReleaseImages(str(row, "release_mbid"), 0);
        out.push_back(std::move(t));
    }
    return out;
}

std::vector<Album> parseFreshReleases(const json& root, int limit) {
    struct Row {
        Album album;
        bool cover;
        bool longForm;
        int64_t listens;
    };
    std::vector<Row> rows;
    std::unordered_set<std::string> seen;
    for (const auto& r : lbPayloadList(root, "releases")) {
        Album a;
        a.id = str(r, "release_group_mbid");
        if (a.id.empty() || !seen.insert(a.id).second) continue;
        a.releaseId = str(r, "release_mbid");
        a.name = str(r, "release_name");
        a.primaryType = str(r, "release_group_primary_type");
        if (auto s = str(r, "release_group_secondary_type"); !s.empty()) a.secondaryTypes.push_back(s);
        a.artists = lbArtists(r, "artist_credit_name");
        a.firstReleaseDate = str(r, "release_date");
        const bool cover = num(r, "caa_id") > 0;
        if (cover) a.images = coverArt(a.id);
        const std::string pt = asciiLower(a.primaryType);
        rows.push_back({std::move(a), cover, pt == "album" || pt == "ep", num(r, "listen_count")});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
        if (x.listens != y.listens) return x.listens > y.listens;
        if (x.cover != y.cover) return x.cover;
        if (x.longForm != y.longForm) return x.longForm;
        return x.album.firstReleaseDate > y.album.firstReleaseDate;
    });
    std::vector<Album> out;
    for (auto& r : rows) {
        if (static_cast<int>(out.size()) >= limit) break;
        out.push_back(std::move(r.album));
    }
    return out;
}

std::vector<Artist> parseSimilarArtists(const json& root, const std::string& selfId, int limit) {
    // Labs returns a flat array; older deployments wrapped it as [{ "data": [...] }, ...].
    const json* list = &root;
    if (root.is_array() && !root.empty() && get(root[0], "data").is_array()) list = &get(root[0], "data");
    if (!list->is_array()) list = &lbPayloadList(root, "artists");
    std::vector<std::pair<int64_t, Artist>> rows;
    std::unordered_set<std::string> seen;
    for (const auto& row : *list) {
        Artist a;
        a.id = str(row, "artist_mbid");
        a.name = str(row, "name");
        if (a.name.empty()) a.name = str(row, "artist_name");
        a.type = str(row, "type");
        a.disambiguation = str(row, "comment");
        if (a.id.empty() || a.name.empty() || a.id == selfId || !seen.insert(a.id).second) continue;
        rows.emplace_back(num(row, "score"), std::move(a));
    }
    std::stable_sort(rows.begin(), rows.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
    std::vector<Artist> out;
    for (auto& [s, a] : rows) {
        if (static_cast<int>(out.size()) >= limit) break;
        out.push_back(std::move(a));
    }
    return out;
}

} // namespace st::mb::detail
