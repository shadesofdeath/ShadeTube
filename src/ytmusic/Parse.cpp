#include "ytmusic/Parse.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <optional>
#include <regex>
#include <unordered_set>

namespace st::ytm {

using catalog::Album;
using catalog::AlbumRef;
using catalog::Artist;
using catalog::ArtistRef;
using catalog::CardItem;
using catalog::Image;
using catalog::Playlist;
using catalog::Shelf;
using catalog::Track;

// ---- Ids ---------------------------------------------------------------------------------------------------------

namespace {
std::string after(std::string_view id, std::string_view prefix) {
    return id.size() > prefix.size() && id.substr(0, prefix.size()) == prefix ? std::string(id.substr(prefix.size()))
                                                                               : std::string();
}
} // namespace

bool isYtmId(std::string_view id) { return id.size() > 4 && id.substr(0, 4) == "ytm:"; }
bool isTrackId(std::string_view id) { return id.size() == kTrackPrefix.size() + 11 && id.substr(0, kTrackPrefix.size()) == kTrackPrefix; }
std::string albumId(std::string_view browseId) { return std::string(kAlbumPrefix) + std::string(browseId); }
std::string artistId(std::string_view channelId) { return std::string(kArtistPrefix) + std::string(channelId); }
std::string playlistId(std::string_view id) {
    if (id.size() > 2 && id.substr(0, 2) == "VL") id.remove_prefix(2);
    return std::string(kPlaylistPrefix) + std::string(id);
}
std::string trackId(std::string_view videoId) { return std::string(kTrackPrefix) + std::string(videoId); }
std::string rawAlbum(std::string_view id) { return after(id, kAlbumPrefix); }
std::string rawArtist(std::string_view id) { return after(id, kArtistPrefix); }
std::string rawPlaylist(std::string_view id) { return after(id, kPlaylistPrefix); }
std::string rawVideo(std::string_view id) { return isTrackId(id) ? std::string(id.substr(kTrackPrefix.size())) : std::string(); }

namespace {

// ---- Navigation --------------------------------------------------------------------------------------------------

const Json* step(const Json* j, const char* key) {
    if (!j || !j->is_object()) return nullptr;
    const auto it = j->find(key);
    return it == j->end() ? nullptr : &*it;
}
const Json* step(const Json* j, int i) {
    if (!j || !j->is_array() || i < 0 || static_cast<size_t>(i) >= j->size()) return nullptr;
    return &(*j)[static_cast<size_t>(i)];
}
template <class... P>
const Json* dig(const Json* j, P... path) {
    ((j = step(j, path)), ...);
    return j;
}
template <class... P>
const Json* dig(const Json& j, P... path) {
    return dig(&j, path...);
}

std::string str(const Json* j) { return j && j->is_string() ? j->get<std::string>() : std::string(); }

// Every value stored under one of `keys` below `j`, in document order; a match is not searched further (renderers of
// the same kind never nest in practice, and when they do the outer one is the one meant).
void findAll(const Json& j, std::initializer_list<const char*> keys, std::vector<std::pair<const char*, const Json*>>& out,
             int depth = 0) {
    if (depth > 80) return;
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            const char* hit = nullptr;
            for (const char* k : keys)
                if (it.key() == k) hit = k;
            if (hit) out.emplace_back(hit, &it.value());
            else if (it->is_structured()) findAll(*it, keys, out, depth + 1);
        }
    } else if (j.is_array()) {
        for (const auto& v : j)
            if (v.is_structured()) findAll(v, keys, out, depth + 1);
    }
}
std::vector<const Json*> findAll(const Json& j, const char* key) {
    std::vector<std::pair<const char*, const Json*>> hits;
    findAll(j, {key}, hits);
    std::vector<const Json*> out;
    out.reserve(hits.size());
    for (const auto& h : hits) out.push_back(h.second);
    return out;
}
const Json* findFirst(const Json& j, const char* key, int depth = 0) {
    if (depth > 80) return nullptr;
    if (j.is_object()) {
        if (const auto it = j.find(key); it != j.end()) return &*it;
        for (const auto& v : j)
            if (v.is_structured())
                if (const Json* r = findFirst(v, key, depth + 1)) return r;
    } else if (j.is_array()) {
        for (const auto& v : j)
            if (v.is_structured())
                if (const Json* r = findFirst(v, key, depth + 1)) return r;
    }
    return nullptr;
}
const Json* findFirst(const Json* j, const char* key) { return j ? findFirst(*j, key) : nullptr; }

// ---- Text --------------------------------------------------------------------------------------------------------

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (static_cast<unsigned char>(s[a]) <= ' ')) ++a;
    while (b > a && (static_cast<unsigned char>(s[b - 1]) <= ' ')) --b;
    return std::string(s.substr(a, b - a));
}
std::string lower(std::string_view s) {
    std::string out(s);
    for (auto& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}
constexpr std::string_view kBullet = "\xE2\x80\xA2";   // "•": YouTube Music's separator between subtitle parts
bool isBullet(std::string_view s) { return trim(s) == kBullet; }
bool isJoiner(std::string_view s) {
    const std::string t = lower(trim(s));
    return t == "," || t == "&" || t == "and" || t == "x";
}
bool isYear(std::string_view s) {
    return s.size() == 4 && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; }) && s[0] >= '1' &&
           s[0] <= '2';
}

// The type word YouTube Music puts first in a row's subtitle (English, hl=en).
const char* typeWord(std::string_view s) {
    static constexpr const char* kWords[] = {"Song",    "Video",   "Album",   "Single", "EP",    "Artist",   "Playlist",
                                             "Episode", "Podcast", "Profile", "Station", "Chart", "Audiobook"};
    const std::string t = trim(s);
    for (const char* w : kWords)
        if (t == w) return w;
    return nullptr;
}

// ---- Endpoints and runs ------------------------------------------------------------------------------------------

struct Target {
    std::string browseId, pageType, params;
    std::string videoId, playlistId, videoType;
};

// navigationEndpoint-like object: a browse or watch endpoint, directly or one level down (clickCommand, onTap, ...).
Target target(const Json* ep) {
    Target t;
    if (!ep || !ep->is_object()) return t;
    const Json* b = step(ep, "browseEndpoint");
    const Json* w = step(ep, "watchEndpoint");
    const Json* wp = step(ep, "watchPlaylistEndpoint");
    if (!b && !w && !wp)
        for (const auto& v : *ep)
            if (v.is_object()) {
                if (!b) b = step(&v, "browseEndpoint");
                if (!w) w = step(&v, "watchEndpoint");
            }
    if (b) {
        t.browseId = str(step(b, "browseId"));
        t.params = str(step(b, "params"));
        t.pageType = str(dig(b, "browseEndpointContextSupportedConfigs", "browseEndpointContextMusicConfig", "pageType"));
    }
    if (w) {
        t.videoId = str(step(w, "videoId"));
        t.playlistId = str(step(w, "playlistId"));
        t.videoType = str(dig(w, "watchEndpointMusicSupportedConfigs", "watchEndpointMusicConfig", "musicVideoType"));
    }
    if (wp && t.playlistId.empty()) t.playlistId = str(step(wp, "playlistId"));
    return t;
}

struct Run {
    std::string text;
    Target to;
};

std::vector<Run> runsOf(const Json* node) {
    std::vector<Run> out;
    const Json* runs = step(node, "runs");
    if (!runs || !runs->is_array()) {
        if (const std::string t = text(node); !t.empty()) out.push_back({t, {}});
        return out;
    }
    for (const auto& r : *runs) {
        Run run;
        run.text = str(step(&r, "text"));
        run.to = target(step(&r, "navigationEndpoint"));
        out.push_back(std::move(run));
    }
    return out;
}

bool isArtistTarget(const Target& t) {
    return t.pageType == "MUSIC_PAGE_TYPE_ARTIST" || t.pageType == "MUSIC_PAGE_TYPE_USER_CHANNEL" ||
           (t.pageType.empty() && t.browseId.size() == 24 && t.browseId.rfind("UC", 0) == 0);
}
bool isAlbumTarget(const Target& t) { return t.pageType == "MUSIC_PAGE_TYPE_ALBUM" || t.browseId.rfind("MPREb_", 0) == 0; }

std::vector<std::vector<Run>> segments(const std::vector<Run>& runs) {
    std::vector<std::vector<Run>> out(1);
    for (const auto& r : runs) {
        if (isBullet(r.text)) {
            if (!out.back().empty()) out.emplace_back();
            continue;
        }
        out.back().push_back(r);
    }
    if (out.back().empty()) out.pop_back();
    return out;
}
std::string joined(const std::vector<Run>& seg) {
    std::string s;
    for (const auto& r : seg) s += r.text;
    return trim(s);
}

// Artist credits of one subtitle part: linked artists plus plain-text names, without the joiners between them.
std::vector<ArtistRef> artistsOf(const std::vector<Run>& seg) {
    std::vector<ArtistRef> out;
    for (const auto& r : seg) {
        if (isJoiner(r.text)) continue;
        const std::string name = trim(r.text);
        if (name.empty()) continue;
        if (isArtistTarget(r.to) && r.to.pageType != "MUSIC_PAGE_TYPE_USER_CHANNEL") out.push_back({artistId(r.to.browseId), name});
        else if (isArtistTarget(r.to)) out.push_back({"", name});   // a channel without an artist page: the name only
        else {
            // Plain text: "A, B & C" may come as one run.
            size_t pos = 0;
            const std::string t = r.text;
            while (pos < t.size()) {
                size_t cut = std::string::npos, len = 0;
                for (std::string_view sep : {std::string_view(", "), std::string_view(" & ")}) {
                    const size_t at = t.find(sep, pos);
                    if (at != std::string::npos && at < cut) {
                        cut = at;
                        len = sep.size();
                    }
                }
                const std::string part = trim(std::string_view(t).substr(pos, cut == std::string::npos ? std::string::npos : cut - pos));
                if (!part.empty() && !isJoiner(part)) out.push_back({"", part});
                if (cut == std::string::npos) break;
                pos = cut + len;
            }
        }
    }
    return out;
}

bool hasExplicitBadge(const Json* node) {
    if (!node) return false;
    for (const Json* icon : findAll(*node, "iconType"))
        if (str(icon) == "MUSIC_EXPLICIT_BADGE") return true;
    return false;
}

// ---- Rows --------------------------------------------------------------------------------------------------------

enum class Kind { None, Song, Video, Album, Artist, Playlist, Other };

struct Item {
    Kind kind = Kind::None;
    std::string title;
    Target to;                       // what the row opens / plays
    std::string type;                // the English type word of its subtitle ("Single", "EP", "Song"...)
    std::vector<ArtistRef> artists;
    AlbumRef album;
    int durationMs = 0;
    std::string year;
    std::string subtitle;            // as shown: the parts after the title, " • " joined
    int64_t count = 0;               // "35 songs", "65.4M monthly audience"
    std::string countWord;           // "songs", "monthly audience", "views", "plays", "subscribers"
    std::string owner;               // a playlist's plain-text author ("YouTube Music")
    std::vector<Image> images;
    bool explicitContent = false;
    bool playable = true;
    int index = 0;
};

Kind classify(const Item& it) {
    const Target& t = it.to;
    if (isAlbumTarget(t)) return Kind::Album;
    if (t.pageType == "MUSIC_PAGE_TYPE_ARTIST") return Kind::Artist;
    if (t.pageType == "MUSIC_PAGE_TYPE_PLAYLIST" || (t.pageType.empty() && t.browseId.rfind("VL", 0) == 0)) return Kind::Playlist;
    if (!t.pageType.empty()) return Kind::Other;   // profiles, podcasts, episodes, the library...
    if (!t.videoId.empty()) {
        if (t.videoType == "MUSIC_VIDEO_TYPE_PODCAST_EPISODE" || it.type == "Episode" || it.type == "Podcast") return Kind::Other;
        if (t.videoType == "MUSIC_VIDEO_TYPE_ATV") return Kind::Song;
        if (!t.videoType.empty() || it.type == "Video") return Kind::Video;
        return Kind::Song;
    }
    if (!t.browseId.empty() && t.browseId.size() == 24 && t.browseId.rfind("UC", 0) == 0) return Kind::Artist;
    return Kind::None;
}

// Reads the subtitle parts: the type word, credits, album, duration, year, counts.
void readSubtitle(Item& it, const std::vector<Run>& runs) {
    std::vector<std::string> shown;
    std::vector<std::vector<Run>> plain;   // parts nothing else claimed
    const auto segs = segments(runs);
    for (size_t i = 0; i < segs.size(); ++i) {
        const auto& seg = segs[i];
        const std::string all = joined(seg);
        if (all.empty()) continue;
        shown.push_back(all);
        const bool linked = std::any_of(seg.begin(), seg.end(), [](const Run& r) { return !r.to.browseId.empty(); });
        if (i == 0 && !linked)
            if (const char* w = typeWord(all)) {
                it.type = w;
                continue;
            }
        if (const auto a = std::find_if(seg.begin(), seg.end(), [](const Run& r) { return isAlbumTarget(r.to); }); a != seg.end()) {
            if (it.album.id.empty()) it.album = {albumId(a->to.browseId), trim(a->text), {}};
            continue;
        }
        if (std::any_of(seg.begin(), seg.end(), [](const Run& r) { return isArtistTarget(r.to); })) {
            if (it.artists.empty()) it.artists = artistsOf(seg);
            continue;
        }
        if (const int ms = parseDurationMs(all); ms > 0) {
            if (!it.durationMs) it.durationMs = ms;
            continue;
        }
        if (isYear(all)) {
            it.year = all;
            continue;
        }
        if (all[0] >= '0' && all[0] <= '9') {
            if (const int64_t n = parseCount(all); n > 0 && all.find(' ') != std::string::npos) {
                it.count = n;
                it.countWord = lower(trim(std::string_view(all).substr(all.find(' ') + 1)));
                continue;
            }
        }
        plain.push_back(seg);
    }
    if (!plain.empty()) {
        if (it.artists.empty()) it.artists = artistsOf(plain[0]);
        it.owner = joined(plain[0]);
    }
    std::string sub;
    for (const auto& s : shown) {
        if (!sub.empty()) sub += " \xE2\x80\xA2 ";
        sub += s;
    }
    it.subtitle = std::move(sub);
}

std::vector<Run> concatRuns(const std::vector<std::vector<Run>>& columns) {
    std::vector<Run> out;
    for (const auto& c : columns) {
        if (c.empty()) continue;
        if (!out.empty()) out.push_back({" \xE2\x80\xA2 ", {}});
        out.insert(out.end(), c.begin(), c.end());
    }
    return out;
}

// musicResponsiveListItemRenderer: search results, album / playlist / artist rows, the list carousels of Home.
Item listItem(const Json& r) {
    Item it;
    std::vector<std::vector<Run>> cols;
    if (const Json* flex = step(&r, "flexColumns"); flex && flex->is_array())
        for (const auto& c : *flex) cols.push_back(runsOf(dig(&c, "musicResponsiveListItemFlexColumnRenderer", "text")));
    if (!cols.empty()) {
        for (const auto& run : cols[0]) it.title += run.text;
        it.title = trim(it.title);
        for (const auto& run : cols[0]) {
            if (!run.to.videoId.empty() && it.to.videoId.empty()) {
                it.to.videoId = run.to.videoId;
                it.to.playlistId = run.to.playlistId;
                it.to.videoType = run.to.videoType;
            }
            if (!run.to.browseId.empty() && it.to.browseId.empty()) {
                it.to.browseId = run.to.browseId;
                it.to.pageType = run.to.pageType;
            }
        }
    }
    if (const Target nav = target(step(&r, "navigationEndpoint")); !nav.browseId.empty() || !nav.videoId.empty()) {
        if (!nav.browseId.empty()) {
            it.to.browseId = nav.browseId;
            it.to.pageType = nav.pageType;
            it.to.params = nav.params;
        }
        if (it.to.videoId.empty() && !nav.videoId.empty()) {
            it.to.videoId = nav.videoId;
            it.to.videoType = nav.videoType;
            it.to.playlistId = nav.playlistId;
        }
    }
    if (it.to.videoId.empty()) it.to.videoId = str(dig(&r, "playlistItemData", "videoId"));
    if (const Target play = target(dig(&r, "overlay", "musicItemThumbnailOverlayRenderer", "content", "musicPlayButtonRenderer",
                                       "playNavigationEndpoint"));
        !play.videoId.empty()) {
        if (it.to.videoId.empty()) it.to.videoId = play.videoId;
        if (it.to.videoType.empty() && play.videoId == it.to.videoId) it.to.videoType = play.videoType;
    }
    std::vector<std::vector<Run>> rest(cols.size() > 1 ? cols.begin() + 1 : cols.end(), cols.end());
    readSubtitle(it, concatRuns(rest));
    if (const Json* fixed = step(&r, "fixedColumns"); fixed && fixed->is_array())
        for (const auto& c : *fixed) {
            const std::string t = text(dig(&c, "musicResponsiveListItemFixedColumnRenderer", "text"));
            if (const int ms = parseDurationMs(t); ms > 0 && !it.durationMs) it.durationMs = ms;
        }
    if (const std::string idx = trim(text(step(&r, "index"))); !idx.empty()) {
        int n = 0;
        std::from_chars(idx.data(), idx.data() + idx.size(), n);
        it.index = n;
    }
    it.images = images(step(&r, "thumbnail"));
    it.explicitContent = hasExplicitBadge(step(&r, "badges")) || hasExplicitBadge(step(&r, "subtitleBadges"));
    it.playable = str(step(&r, "musicItemRendererDisplayPolicy")) != "MUSIC_ITEM_RENDERER_DISPLAY_POLICY_GREY_OUT";
    it.kind = classify(it);
    return it;
}

// musicTwoRowItemRenderer: the cards of carousels (albums, playlists, artists, videos).
Item twoRowItem(const Json& r) {
    Item it;
    const auto titleRuns = runsOf(step(&r, "title"));
    for (const auto& run : titleRuns) it.title += run.text;
    it.title = trim(it.title);
    it.to = target(step(&r, "navigationEndpoint"));
    if (it.to.browseId.empty() && it.to.videoId.empty() && !titleRuns.empty()) it.to = titleRuns[0].to;
    readSubtitle(it, runsOf(step(&r, "subtitle")));
    it.images = images(step(&r, "thumbnailRenderer"));
    it.explicitContent = hasExplicitBadge(step(&r, "subtitleBadges"));
    it.kind = classify(it);
    return it;
}

// musicCardShelfRenderer: the "top result" card of an unfiltered search.
Item cardItem(const Json& r) {
    Item it;
    const auto titleRuns = runsOf(step(&r, "title"));
    for (const auto& run : titleRuns) it.title += run.text;
    it.title = trim(it.title);
    if (!titleRuns.empty()) it.to = titleRuns[0].to;
    if (it.to.browseId.empty() && it.to.videoId.empty()) it.to = target(step(&r, "onTap"));
    readSubtitle(it, runsOf(step(&r, "subtitle")));
    it.images = images(step(&r, "thumbnail"));
    it.kind = classify(it);
    return it;
}

// playlistPanelVideoRenderer: a queue row of "next" (radio).
Item panelItem(const Json& r) {
    Item it;
    it.title = trim(text(step(&r, "title")));
    it.to = target(step(&r, "navigationEndpoint"));
    if (it.to.videoId.empty()) it.to.videoId = str(step(&r, "videoId"));
    readSubtitle(it, runsOf(step(&r, "longBylineText")));
    if (it.artists.empty()) it.artists = artistsOf(runsOf(step(&r, "shortBylineText")));
    if (const int ms = parseDurationMs(text(step(&r, "lengthText"))); ms > 0) it.durationMs = ms;
    it.images = images(step(&r, "thumbnail"));
    it.explicitContent = hasExplicitBadge(step(&r, "badges"));
    it.playable = !step(&r, "unplayableText");
    it.kind = it.to.videoId.empty() ? Kind::None : classify(it);
    return it;
}

// ---- Rows -> models ----------------------------------------------------------------------------------------------

std::optional<Track> toTrack(const Item& it, const AlbumRef* album = nullptr, const std::vector<ArtistRef>* fallbackArtists = nullptr) {
    if (it.to.videoId.size() != 11 || (it.kind != Kind::Song && it.kind != Kind::Video)) return std::nullopt;
    Track t;
    t.id = trackId(it.to.videoId);
    t.videoId = it.to.videoId;
    t.name = it.title;
    t.artists = it.artists;
    if (t.artists.empty() && fallbackArtists) t.artists = *fallbackArtists;
    t.album = album ? *album : it.album;
    if (!it.images.empty() && (!album || album->images.empty())) t.album.images = it.images;
    t.durationMs = it.durationMs;
    t.explicitContent = it.explicitContent;
    t.playable = it.playable;
    t.trackNumber = it.index;
    return t;
}

std::string primaryType(const std::string& type) { return type == "Single" ? "Single" : type == "EP" ? "EP" : "Album"; }

Album toAlbum(const Item& it, const std::vector<ArtistRef>* fallbackArtists = nullptr) {
    Album a;
    a.id = albumId(it.to.browseId);
    a.name = it.title;
    a.primaryType = primaryType(it.type);
    a.artists = it.artists;
    if (a.artists.empty() && fallbackArtists) a.artists = *fallbackArtists;
    a.images = it.images;
    a.firstReleaseDate = it.year;
    return a;
}

bool isAudience(const std::string& word) {
    return word.find("audience") != std::string::npos || word.find("listener") != std::string::npos;
}

Artist toArtist(const Item& it) {
    Artist a;
    a.id = artistId(it.to.browseId);
    a.name = it.title;
    for (auto& img : it.images) a.images.push_back(img);
    if (isAudience(it.countWord)) a.listenCount = it.count;
    return a;
}

bool isTrackCount(const std::string& word) {
    for (const char* w : {"song", "track", "video", "episode"})
        if (word.find(w) != std::string::npos) return true;
    return false;
}

Playlist toPlaylist(const Item& it) {
    Playlist p;
    p.id = playlistId(it.to.browseId);
    p.name = it.title;
    p.images = it.images;
    p.ownerName = !it.artists.empty() ? it.artists[0].name : it.owner;
    if (isTrackCount(it.countWord)) p.totalTracks = static_cast<int>(it.count);
    else p.countKnown = false;
    return p;
}

std::optional<CardItem> toCard(const Item& it) {
    CardItem c;
    c.title = it.title;
    c.subtitle = it.subtitle;
    c.images = it.images;
    switch (it.kind) {
    case Kind::Song:
    case Kind::Video: {
        auto t = toTrack(it);
        if (!t) return std::nullopt;
        c.kind = CardItem::Kind::Track;
        c.id = t->id;
        c.track = std::move(*t);
        if (c.subtitle.empty()) c.subtitle = c.track.artistLine();
        break;
    }
    case Kind::Album:
        c.kind = CardItem::Kind::Album;
        c.id = albumId(it.to.browseId);
        break;
    case Kind::Artist:
        c.kind = CardItem::Kind::Artist;
        c.id = artistId(it.to.browseId);
        break;
    case Kind::Playlist:
        c.kind = CardItem::Kind::Playlist;
        c.id = playlistId(it.to.browseId);
        break;
    default: return std::nullopt;
    }
    if (c.title.empty() || c.id.empty()) return std::nullopt;
    return c;
}

// The rows of a list container (contents / items / continuationItems), any of the row renderers.
std::vector<Item> rowsOf(const Json* list) {
    std::vector<Item> out;
    if (!list || !list->is_array()) return out;
    for (const auto& entry : *list) {
        if (const Json* r = step(&entry, "musicResponsiveListItemRenderer")) out.push_back(listItem(*r));
        else if (const Json* c = step(&entry, "musicTwoRowItemRenderer")) out.push_back(twoRowItem(*c));
        else if (const Json* p = step(&entry, "playlistPanelVideoRenderer")) out.push_back(panelItem(*p));
        else if (const Json* w = dig(&entry, "playlistPanelVideoWrapperRenderer", "primaryRenderer", "playlistPanelVideoRenderer"))
            out.push_back(panelItem(*w));
    }
    return out;
}

// A continuation token in a list or its shelf: new style (continuationItemRenderer, sent in the body) or old style
// (nextContinuationData / nextRadioContinuationData, sent as URL parameters).
void continuationOf(const Json* node, std::string& token, bool& inUrl) {
    if (!node) return;
    if (const Json* c = findFirst(*node, "continuationCommand")) {
        if (const std::string t = str(step(c, "token")); !t.empty()) {
            token = t;
            inUrl = false;
            return;
        }
    }
    for (const char* key : {"nextContinuationData", "nextRadioContinuationData"})
        if (const Json* c = findFirst(*node, key)) {
            if (const std::string t = str(step(c, "continuation")); !t.empty()) {
                token = t;
                inUrl = true;
                return;
            }
        }
}

const Json* headerOf(const Json& response, std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        if (const Json* h = findFirst(response, k)) return h;
    return nullptr;
}

std::string canonicalList(const Json& response) {
    const std::string url = str(dig(findFirst(response, "microformatDataRenderer"), "urlCanonical"));
    const size_t at = url.find("list=");
    if (at == std::string::npos) return {};
    std::string id = url.substr(at + 5);
    if (const size_t amp = id.find('&'); amp != std::string::npos) id.resize(amp);
    return id;
}

} // namespace

// ---- Pieces ------------------------------------------------------------------------------------------------------

std::string text(const Json* node) {
    if (!node) return {};
    if (node->is_string()) return node->get<std::string>();
    if (!node->is_object()) return {};
    if (const Json* runs = step(node, "runs"); runs && runs->is_array()) {
        std::string s;
        for (const auto& r : *runs) s += str(step(&r, "text"));
        return s;
    }
    if (const Json* s = step(node, "simpleText")) return str(s);
    if (const Json* s = step(node, "content")) return str(s);   // view models: {"content": "..."}
    return {};
}

int parseDurationMs(std::string_view s) {
    const std::string t = trim(s);
    if (t.empty() || t.size() > 9) return 0;
    int parts[3] = {}, n = 0, cur = 0;
    bool digit = false;
    for (char c : t) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            digit = true;
        } else if (c == ':' && digit && n < 2) {
            parts[n++] = cur;
            cur = 0;
            digit = false;
        } else {
            return 0;
        }
    }
    if (!digit || n == 0) return 0;
    parts[n++] = cur;
    int sec = 0;
    for (int i = 0; i < n; ++i) sec = sec * 60 + parts[i];
    return sec * 1000;
}

int64_t parseCount(std::string_view s) {
    const std::string t = trim(s);
    double value = 0;
    size_t i = 0;
    bool any = false, frac = false;
    double scale = 0.1;
    for (; i < t.size(); ++i) {
        const char c = t[i];
        if (c >= '0' && c <= '9') {
            any = true;
            if (frac) {
                value += (c - '0') * scale;
                scale /= 10;
            } else {
                value = value * 10 + (c - '0');
            }
        } else if (c == ',' && !frac) {
            continue;   // thousands separator (hl=en)
        } else if (c == '.' && !frac) {
            frac = true;
        } else {
            break;
        }
    }
    if (!any) return 0;
    if (i < t.size()) {
        const char m = t[i];
        if (m == 'K') value *= 1e3;
        else if (m == 'M') value *= 1e6;
        else if (m == 'B') value *= 1e9;
    }
    return static_cast<int64_t>(std::llround(value));
}

std::vector<Image> images(const Json* node, bool square) {
    std::vector<Image> out;
    const Json* thumbs = node ? findFirst(*node, "thumbnails") : nullptr;
    if (!thumbs || !thumbs->is_array()) {
        // View models: {"sources": [{"url", "width", "height"}]}.
        thumbs = node ? findFirst(*node, "sources") : nullptr;
        if (!thumbs || !thumbs->is_array()) return out;
    }
    for (const auto& t : *thumbs) {
        Image img;
        img.url = str(step(&t, "url"));
        if (img.url.empty()) continue;
        if (img.url.rfind("//", 0) == 0) img.url = "https:" + img.url;
        if (const Json* w = step(&t, "width"); w && w->is_number()) img.width = w->get<int>();
        if (const Json* h = step(&t, "height"); h && h->is_number()) img.height = h->get<int>();
        out.push_back(std::move(img));
    }
    std::sort(out.begin(), out.end(), [](const Image& a, const Image& b) { return a.width < b.width; });
    if (out.empty()) return out;
    // googleusercontent / ggpht pictures carry their size in the URL ("=w60-h60-l90-rj", "=s192"): any size can be had.
    const Image big = out.back();
    static const std::regex wh(R"(=w(\d{1,4})-h(\d{1,4})([^/=]*)$)");
    static const std::regex sz(R"(=s(\d{1,4})([^/=]*)$)");
    std::smatch m;
    const bool google = big.url.find("googleusercontent.com") != std::string::npos || big.url.find("ggpht.com") != std::string::npos;
    if (!google) return out;
    std::string base, rest;
    int bw = 0, bh = 0;
    bool sForm = false;   // "=s<n>": a square of that size
    if (std::regex_search(big.url, m, wh)) {
        base = big.url.substr(0, static_cast<size_t>(m.position(0)));
        bw = std::stoi(m[1].str());
        bh = std::stoi(m[2].str());
        rest = m[3].str();
    } else if (std::regex_search(big.url, m, sz)) {
        base = big.url.substr(0, static_cast<size_t>(m.position(0)));
        bw = bh = std::stoi(m[1].str());
        rest = m[2].str();
        sForm = true;
    } else {
        return out;
    }
    auto sized = [&](int w, int h) {
        if (sForm) return Image{base + "=s" + std::to_string(w) + rest, w, w};
        std::string r = rest;
        if (square && r.find("-p") == std::string::npos) r = "-p" + r;   // a centered crop, not a squeezed banner
        return Image{base + "=w" + std::to_string(w) + "-h" + std::to_string(h) + r, w, h};
    };
    if (bw <= 0 || bh <= 0) return out;
    if (square && bw != bh) {
        // A banner asked for as a square picture: square crops of it only.
        out.clear();
        for (int s : {120, 240, 544, 1080}) out.push_back(sized(s, s));
        return out;
    }
    for (int w : {544, 1080}) {
        if (w <= big.width) continue;
        const int h = static_cast<int>(std::lround(static_cast<double>(w) * bh / bw));
        out.push_back(sized(w, h));
    }
    return out;
}

// ---- Search ------------------------------------------------------------------------------------------------------

catalog::SearchResults parseSearch(const Json& response) {
    catalog::SearchResults out;
    std::unordered_set<std::string> seen;
    std::vector<std::pair<const char*, const Json*>> hits;
    findAll(response, {"musicResponsiveListItemRenderer", "musicCardShelfRenderer"}, hits);
    std::vector<Item> items;
    size_t topCard = SIZE_MAX;   // the index of the top result card's own item
    for (const auto& [key, node] : hits) {
        if (std::string_view(key) == "musicCardShelfRenderer") {
            if (topCard == SIZE_MAX) topCard = items.size();
            items.push_back(cardItem(*node));
            // The card's own rows (songs of the top result) come after it.
            std::vector<std::pair<const char*, const Json*>> inner;
            findAll(*node, {"musicResponsiveListItemRenderer"}, inner);
            for (const auto& [k, row] : inner) items.push_back(listItem(*row));
        } else {
            items.push_back(listItem(*node));
        }
    }
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        std::string id;
        switch (it.kind) {
        case Kind::Song:
            if (auto t = toTrack(it)) {
                id = t->id;
                if (seen.insert(t->id).second) out.tracks.push_back(std::move(*t));
            }
            break;
        case Kind::Album:
            if (auto a = toAlbum(it); !it.to.browseId.empty()) {
                id = a.id;
                if (seen.insert(a.id).second) out.albums.push_back(std::move(a));
            }
            break;
        case Kind::Artist:
            if (auto a = toArtist(it); !it.to.browseId.empty()) {
                id = a.id;
                if (seen.insert(a.id).second) out.artists.push_back(std::move(a));
            }
            break;
        case Kind::Playlist:
            if (auto p = toPlaylist(it); !it.to.browseId.empty()) {
                id = p.id;
                if (seen.insert(p.id).second) out.playlists.push_back(std::move(p));
            }
            break;
        default: break;
        }
        if (i == topCard) out.topResult = id;   // "" when the card is something the lists leave out (a video)
    }
    return out;
}

SearchPage parseSearchPage(const Json& response) {
    SearchPage out;
    out.results = parseSearch(response);
    bool inUrl = true;
    if (const Json* c = findFirst(response, "musicShelfContinuation")) continuationOf(step(c, "continuations"), out.continuation, inUrl);
    if (out.continuation.empty())
        for (const Json* shelf : findAll(response, "musicShelfRenderer")) {
            continuationOf(step(shelf, "continuations"), out.continuation, inUrl);
            if (!out.continuation.empty()) break;
        }
    return out;
}

ReleasesPage parseReleases(const Json& response, const catalog::ArtistRef& artist) {
    ReleasesPage out;
    const std::vector<ArtistRef> self{artist};
    std::unordered_set<std::string> seen;
    std::vector<std::pair<const char*, const Json*>> hits;
    findAll(response, {"gridRenderer", "gridContinuation", "musicCarouselShelfRenderer"}, hits);
    for (const auto& [key, node] : hits) {
        const Json* list = step(node, "items");
        if (!list) list = step(node, "contents");
        for (const auto& it : rowsOf(list))
            if (it.kind == Kind::Album)
                if (Album a = toAlbum(it, &self); seen.insert(a.id).second) out.releases.push_back(std::move(a));
        bool inUrl = true;
        if (out.continuation.empty()) continuationOf(step(node, "continuations"), out.continuation, inUrl);
    }
    return out;
}

// ---- Album -------------------------------------------------------------------------------------------------------

Album parseAlbum(const Json& response, std::string_view browseId) {
    Album a;
    a.id = albumId(browseId);
    const Json* header = headerOf(response, {"musicResponsiveHeaderRenderer", "musicDetailHeaderRenderer"});
    std::string type;
    if (header) {
        a.name = trim(text(step(header, "title")));
        Item sub;
        readSubtitle(sub, runsOf(step(header, "subtitle")));
        type = sub.type;
        a.firstReleaseDate = sub.year;
        Item strap;
        readSubtitle(strap, runsOf(step(header, "straplineTextOne")));
        a.artists = !strap.artists.empty() ? strap.artists : sub.artists;
        a.images = images(step(header, "thumbnail"));
    }
    if (a.name.empty()) a.name = str(dig(findFirst(response, "microformatDataRenderer"), "title"));
    a.primaryType = primaryType(type);
    a.releaseId = canonicalList(response);
    if (a.releaseId.empty() && header)
        if (const Json* wp = findFirst(*header, "watchPlaylistEndpoint")) a.releaseId = str(step(wp, "playlistId"));
    const AlbumRef ref{a.id, a.name, a.images};
    for (const Json* shelf : findAll(response, "musicShelfRenderer"))
        for (const auto& it : rowsOf(step(shelf, "contents")))
            if (auto t = toTrack(it, &ref, &a.artists)) a.tracks.push_back(std::move(*t));
    for (size_t i = 0; i < a.tracks.size(); ++i)
        if (a.tracks[i].trackNumber <= 0) a.tracks[i].trackNumber = static_cast<int>(i) + 1;
    a.totalTracks = static_cast<int>(a.tracks.size());
    return a;
}

// ---- Artist ------------------------------------------------------------------------------------------------------

ArtistResult parseArtist(const Json& response, std::string_view channelId) {
    ArtistResult out;
    auto& page = out.page;
    auto& artist = page.artist;
    artist.id = artistId(channelId);
    const Json* header = headerOf(response, {"musicImmersiveHeaderRenderer", "musicVisualHeaderRenderer", "musicResponsiveHeaderRenderer"});
    if (header) {
        artist.name = trim(text(step(header, "title")));
        artist.images = images(step(header, "thumbnail"), true);
        if (artist.images.empty()) artist.images = images(step(header, "foregroundThumbnail"), true);
        // The header picture is a wide banner (1080 x 450): the page's banner as it is, the avatar a square crop of it.
        if (auto wide = images(step(header, "thumbnail")); !wide.empty() && wide.back().width > wide.back().height * 13 / 10)
            page.banner = std::move(wide);
        page.monthlyListeners = parseCount(text(step(header, "monthlyListenerCount")));
        page.followers = parseCount(text(dig(header, "subscriptionButton", "subscribeButtonRenderer", "subscriberCountText")));
        // The description: Wikipedia's summary (its credit line names the article) or the artist's own text.
        const auto descRuns = runsOf(step(header, "description"));
        std::string bio;
        for (const auto& r : descRuns) bio += r.text;
        if (const size_t wiki = bio.find("\n\nFrom Wikipedia"); wiki != std::string::npos) {
            page.bioSource = "Wikipedia";
            for (const auto& r : descRuns)
                if (r.text.rfind("https://", 0) == 0 && r.text.find("wikipedia.org") != std::string::npos) page.bioUrl = r.text;
            bio.resize(wiki);
        } else if (!bio.empty()) {
            page.bioSource = "YouTube Music";
        }
        page.bio = trim(bio);
        const Target radio = target(dig(header, "startRadioButton", "buttonRenderer", "navigationEndpoint"));
        out.radioVideoId = radio.videoId;
        out.radioPlaylistId = radio.playlistId;
    }
    if (artist.name.empty()) artist.name = str(dig(findFirst(response, "microformatDataRenderer"), "title"));
    const std::vector<ArtistRef> self{{artist.id, artist.name}};

    // Top songs: the first list shelf.
    if (const Json* shelf = findFirst(response, "musicShelfRenderer")) {
        const auto title = runsOf(step(shelf, "title"));
        if (!title.empty() && !title[0].to.browseId.empty()) out.songsPlaylistId = rawPlaylist(playlistId(title[0].to.browseId));
        for (const auto& it : rowsOf(step(shelf, "contents")))
            if (auto t = toTrack(it, nullptr, &self)) page.topTracks.push_back(std::move(*t));
    }

    // Carousels: albums, singles & EPs, appears on, related artists, playlists (the titles are English: hl=en).
    int albumShelves = 0;
    std::unordered_set<std::string> playlistsSeen;
    for (const Json* shelf : findAll(response, "musicCarouselShelfRenderer")) {
        const Json* head = dig(shelf, "header", "musicCarouselShelfBasicHeaderRenderer");
        const std::string title = lower(text(step(head, "title")));
        // "More" (or the title itself) opens the whole group: the artist's discography page.
        Target more = target(dig(head, "moreContentButton", "buttonRenderer", "navigationEndpoint"));
        if (more.browseId.empty())
            if (const auto t = runsOf(step(head, "title")); !t.empty()) more = t[0].to;
        const bool discography = more.pageType == "MUSIC_PAGE_TYPE_ARTIST_DISCOGRAPHY" || more.browseId.rfind("MPAD", 0) == 0;
        const auto rows = rowsOf(step(shelf, "contents"));
        size_t albums = 0, artists = 0, singles = 0, playlists = 0;
        for (const auto& it : rows) {
            if (it.kind == Kind::Album) ++albums;
            if (it.kind == Kind::Artist) ++artists;
            if (it.kind == Kind::Playlist) ++playlists;
            if (it.type == "Single" || it.type == "EP") ++singles;
        }
        if (albums > 0 && albums >= rows.size() / 2) {
            std::vector<Album>* into = nullptr;
            const bool singleTitle = title.find("single") != std::string::npos || title == "eps";
            const bool appears = title.find("appears") != std::string::npos;
            if (appears) into = &page.appearsOn;
            else if (singleTitle) into = &page.singles;
            else if (title.find("album") != std::string::npos) into = &page.albums;
            else if (title.empty() || albumShelves < 2) into = singles == albums ? &page.singles : &page.albums;   // unknown words
            else into = &page.other;
            if (!appears) ++albumShelves;
            if (discography && into == &page.albums) out.allAlbums = {more.browseId, more.params};
            if (discography && into == &page.singles) out.allSingles = {more.browseId, more.params};
            for (const auto& it : rows) {
                if (it.kind != Kind::Album) continue;
                Album a = toAlbum(it, appears ? nullptr : &self);   // appears on: other artists' releases
                if (into == &page.singles && a.primaryType == "Album") a.primaryType = "Single";
                into->push_back(std::move(a));
            }
        } else if (artists > 0 && artists >= rows.size() / 2 && page.similar.empty()) {
            for (const auto& it : rows)
                if (it.kind == Kind::Artist) page.similar.push_back(toArtist(it));
        } else if (playlists > 0 && playlists >= rows.size() / 2) {   // "Featured on", "Playlists by <artist>"
            for (const auto& it : rows)
                if (it.kind == Kind::Playlist)
                    if (Playlist p = toPlaylist(it); playlistsSeen.insert(p.id).second) page.playlists.push_back(std::move(p));
        }
    }
    return out;
}

// ---- Playlist ----------------------------------------------------------------------------------------------------

PlaylistPage parsePlaylist(const Json& response, std::string_view id) {
    PlaylistPage out;
    Playlist& p = out.playlist;
    p.id = playlistId(id);
    const Json* header = headerOf(response, {"musicResponsiveHeaderRenderer", "musicDetailHeaderRenderer"});
    if (header) {
        p.name = trim(text(step(header, "title")));
        p.description = trim(text(dig(header, "description", "musicDescriptionShelfRenderer", "description")));
        if (p.description.empty()) p.description = trim(text(step(header, "description")));
        p.images = images(step(header, "thumbnail"));
        // Author: the strapline (editorial lists), the avatar stack (community lists) or the subtitle (older header).
        std::string owner = trim(text(step(header, "straplineTextOne")));
        if (owner.empty()) owner = trim(text(dig(header, "facepile", "avatarStackViewModel", "text")));
        Item sub;
        readSubtitle(sub, runsOf(step(header, "subtitle")));
        if (owner.empty()) owner = !sub.artists.empty() ? sub.artists[0].name : sub.owner;
        p.ownerName = owner;
        // "6.5K views • 36 tracks • 2 hours": the track count is not the first number.
        p.countKnown = false;
        for (const auto& seg : segments(runsOf(step(header, "secondSubtitle")))) {
            const std::string s = joined(seg);
            const size_t sp = s.find(' ');
            if (sp != std::string::npos && isTrackCount(lower(s.substr(sp + 1)))) {
                p.totalTracks = static_cast<int>(parseCount(s));
                p.countKnown = true;
            }
        }
    }
    if (p.name.empty()) p.name = str(dig(findFirst(response, "microformatDataRenderer"), "title"));
    std::vector<const Json*> shelves = findAll(response, "musicPlaylistShelfRenderer");
    if (shelves.empty()) shelves = findAll(response, "musicShelfRenderer");
    for (const Json* shelf : shelves) {
        for (const auto& it : rowsOf(step(shelf, "contents")))
            if (auto t = toTrack(it)) out.tracks.push_back(std::move(*t));
        if (out.continuation.empty()) {
            continuationOf(step(shelf, "contents"), out.continuation, out.continuationInUrl);
            if (out.continuation.empty()) continuationOf(step(shelf, "continuations"), out.continuation, out.continuationInUrl);
        }
    }
    if (!p.countKnown && out.continuation.empty()) {   // everything is here: the count is what we have
        p.totalTracks = static_cast<int>(out.tracks.size());
        p.countKnown = true;
    }
    return out;
}

PlaylistPage parsePlaylistContinuation(const Json& response) {
    PlaylistPage out;
    const Json* list = nullptr;
    for (const Json* a : findAll(response, "appendContinuationItemsAction"))
        if ((list = step(a, "continuationItems"))) break;
    if (!list) list = dig(findFirst(response, "musicPlaylistShelfContinuation"), "contents");
    if (!list) list = dig(findFirst(response, "musicShelfContinuation"), "contents");
    for (const auto& it : rowsOf(list))
        if (auto t = toTrack(it)) out.tracks.push_back(std::move(*t));
    continuationOf(list, out.continuation, out.continuationInUrl);
    if (out.continuation.empty())
        for (const char* k : {"musicPlaylistShelfContinuation", "musicShelfContinuation"})
            if (const Json* c = findFirst(response, k)) continuationOf(step(c, "continuations"), out.continuation, out.continuationInUrl);
    return out;
}

// ---- Watch next (radio) ------------------------------------------------------------------------------------------

namespace {
WatchPage watchFrom(const Json* panel) {
    WatchPage out;
    if (!panel) return out;
    std::unordered_set<std::string> seen;
    for (const auto& it : rowsOf(step(panel, "contents")))
        if (auto t = toTrack(it); t && seen.insert(t->id).second) out.tracks.push_back(std::move(*t));
    bool inUrl = true;
    continuationOf(step(panel, "continuations"), out.continuation, inUrl);
    return out;
}
} // namespace

WatchPage parseWatch(const Json& response) { return watchFrom(findFirst(response, "playlistPanelRenderer")); }
WatchPage parseWatchContinuation(const Json& response) { return watchFrom(findFirst(response, "playlistPanelContinuation")); }

// ---- Shelves (Home, explore, charts, mood categories) ------------------------------------------------------------

namespace {
void addShelf(std::vector<Shelf>& out, const std::string& title, const std::string& label, const Json* list) {
    Shelf s;
    s.title = trim(title);
    s.label = trim(label);
    std::unordered_set<std::string> seen;
    for (const auto& it : rowsOf(list))
        if (auto c = toCard(it); c && seen.insert(c->id).second) s.items.push_back(std::move(*c));
    if (!s.items.empty()) out.push_back(std::move(s));
}

void shelvesIn(const Json& root, std::vector<Shelf>& out) {
    std::vector<std::pair<const char*, const Json*>> hits;
    findAll(root, {"musicCarouselShelfRenderer", "gridRenderer", "musicShelfRenderer"}, hits);
    for (const auto& [key, node] : hits) {
        const std::string_view k(key);
        if (k == "musicCarouselShelfRenderer") {
            const Json* h = dig(node, "header", "musicCarouselShelfBasicHeaderRenderer");
            addShelf(out, text(step(h, "title")), text(step(h, "strapline")), step(node, "contents"));
        } else if (k == "gridRenderer") {
            addShelf(out, text(dig(node, "header", "gridHeaderRenderer", "title")), {}, step(node, "items"));
        } else {
            addShelf(out, text(step(node, "title")), {}, step(node, "contents"));
        }
    }
}
} // namespace

ShelvesPage parseShelves(const Json& response) {
    ShelvesPage out;
    shelvesIn(response, out.shelves);
    bool inUrl = true;
    if (const Json* list = findFirst(response, "sectionListRenderer")) continuationOf(step(list, "continuations"), out.continuation, inUrl);
    return out;
}

ShelvesPage parseShelvesContinuation(const Json& response) {
    ShelvesPage out;
    const Json* cont = findFirst(response, "sectionListContinuation");
    if (!cont) return out;
    shelvesIn(*cont, out.shelves);
    bool inUrl = true;
    continuationOf(step(cont, "continuations"), out.continuation, inUrl);
    return out;
}

std::vector<Mood> parseMoods(const Json& response) {
    std::vector<Mood> out;
    for (const Json* grid : findAll(response, "gridRenderer")) {
        const std::string section = trim(text(dig(grid, "header", "gridHeaderRenderer", "title")));
        const Json* items = step(grid, "items");
        if (!items || !items->is_array()) continue;
        for (const auto& entry : *items) {
            const Json* b = step(&entry, "musicNavigationButtonRenderer");
            if (!b) continue;
            Mood m;
            m.section = section;
            m.title = trim(text(step(b, "buttonText")));
            const Target t = target(step(b, "clickCommand"));
            m.params = t.params;
            if (const Json* c = dig(b, "solid", "leftStripeColor"); c && c->is_number())
                m.color = static_cast<uint32_t>(c->get<int64_t>() & 0xFFFFFF);
            if (!m.title.empty() && !m.params.empty()) out.push_back(std::move(m));
        }
    }
    return out;
}

} // namespace st::ytm
