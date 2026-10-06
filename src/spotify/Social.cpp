// Spotify's social surfaces: friend activity (the spclient buddylist) and new releases (the "What's New" feed, with
// the artist overview as the per-artist source). Part of Api (SpotifyApi.h); the parsers are static so tests can feed
// them fixtures.
#include "spotify/SpotifyApi.h"

#include "core/Log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

namespace st::spotify {
namespace {

using json = nlohmann::json;

// Small JSON helpers that never throw on unexpected shapes (as in SpotifyApi.cpp).
const json& at(const json& j, const char* key) {
    static const json null;
    if (j.is_object()) {
        auto it = j.find(key);
        if (it != j.end()) return *it;
    }
    return null;
}
std::string str(const json& j, const char* key) {
    const json& v = at(j, key);
    return v.is_string() ? v.get<std::string>() : std::string{};
}
int integer(const json& j, const char* key, int def = 0) {
    const json& v = at(j, key);
    return v.is_number_integer() ? v.get<int>() : def;
}

// i.scdn.co serves the same image over https; the buddylist still hands out http:// URLs.
std::string httpsUrl(std::string url) {
    if (url.rfind("http://", 0) == 0) url.insert(4, "s");
    return url;
}

std::vector<Image> imagesFrom(const json& sources) {
    std::vector<Image> out;
    if (!sources.is_array()) return out;
    for (const auto& s : sources) {
        Image im;
        im.url = str(s, "url");
        if (at(s, "width").is_number()) im.width = at(s, "width").get<int>();
        if (at(s, "height").is_number()) im.height = at(s, "height").get<int>();
        if (!im.url.empty()) out.push_back(std::move(im));
    }
    return out;
}

std::vector<ArtistRef> artistRefsFrom(const json& artists) {
    std::vector<ArtistRef> out;
    const json& items = artists.is_array() ? artists : at(artists, "items");
    if (!items.is_array()) return out;
    for (const auto& a : items) {
        const json& d = at(a, "data").is_object() ? at(a, "data") : a;
        ArtistRef r{str(d, "uri"), str(at(d, "profile"), "name")};
        if (r.name.empty()) r.name = str(d, "name");
        if (!r.name.empty()) out.push_back(std::move(r));
    }
    return out;
}

// A release date as the catalog keeps it: {isoString:"2026-10-03T00:00:00Z"} or {year, month, day, precision} ->
// "2026-10-03" (DAY), "2026-10" (MONTH), "2026" (YEAR).
std::string releaseDate(const json& date) {
    if (!date.is_object()) return {};
    const std::string iso = str(date, "isoString");
    const std::string precision = str(date, "precision");
    if (iso.size() >= 10) {
        if (precision == "YEAR") return iso.substr(0, 4);
        if (precision == "MONTH") return iso.substr(0, 7);
        return iso.substr(0, 10);
    }
    const int y = integer(date, "year"), m = integer(date, "month"), d = integer(date, "day");
    if (y <= 0) return {};
    char buf[16];
    if (m >= 1 && m <= 12 && d >= 1 && d <= 31 && precision != "YEAR" && precision != "MONTH")
        std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", y, m, d);
    else if (m >= 1 && m <= 12 && precision != "YEAR")
        std::snprintf(buf, sizeof buf, "%04d-%02d", y, m);
    else
        std::snprintf(buf, sizeof buf, "%04d", y);
    return buf;
}

// Spotify's release type ("ALBUM", "SINGLE", "EP", "COMPILATION") in the catalog's spelling. Without one the track
// count decides the way Spotify labels releases (1-3 tracks a single, 4-6 an EP); unknown counts as an album.
std::string releaseType(const std::string& type, int tracks) {
    if (type == "SINGLE") return "Single";
    if (type == "EP") return "EP";
    if (type == "COMPILATION") return "Compilation";
    if (type == "ALBUM") return "Album";
    if (tracks >= 1 && tracks <= 3) return "Single";
    if (tracks >= 4 && tracks <= 6) return "EP";
    return "Album";
}

// An Album / release object (feed item data, discography entries) -> Album without tracks.
Album releaseFrom(const json& d) {
    Album a;
    a.id = str(d, "uri");
    a.name = str(d, "name");
    a.label = str(d, "label");
    a.firstReleaseDate = releaseDate(at(d, "date"));
    if (a.firstReleaseDate.empty()) a.firstReleaseDate = releaseDate(at(d, "releaseDate"));
    a.images = imagesFrom(at(at(d, "coverArt"), "sources"));
    a.artists = artistRefsFrom(at(d, "artists"));
    a.totalTracks = integer(at(d, "tracks"), "totalCount");
    if (a.totalTracks == 0) a.totalTracks = integer(at(d, "tracksV2"), "totalCount");
    a.primaryType = releaseType(str(d, "type"), a.totalTracks);
    return a;
}

// "spotify:user:<name>:playlist:<id>" (the buddylist's legacy form) -> "spotify:playlist:<id>".
std::string normalizeContextUri(const std::string& uri) {
    constexpr std::string_view user = "spotify:user:";
    if (uri.rfind(user, 0) == 0) {
        const size_t pl = uri.find(":playlist:");
        if (pl != std::string::npos) return "spotify:playlist:" + uri.substr(pl + 10);
    }
    return uri;
}

} // namespace

int parseRetryAfter(std::string_view value) {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) value.remove_suffix(1);
    if (value.empty() || value.size() > 9) return 0;
    int n = 0;
    for (const char ch : value) {
        if (ch < '0' || ch > '9') return 0;
        n = n * 10 + (ch - '0');
    }
    return std::min(n, 86400);
}

// ---- Friend activity ----------------------------------------------------------------------------------------------
// GET https://guc-spclient.spotify.com/presence-view/v1/buddylist with the web-player token (what the desktop client's
// Friend Activity panel shows; github.com/valeriangalliat/spotify-buddylist documents the shape). The web player's own
// spclient host answers the same request; it is the fallback when the first one fails with anything but a 429.
std::vector<FriendActivity> Api::friendActivity(const CT& ct) {
    json j;
    try {
        j = spclient("GET", "https://guc-spclient.spotify.com/presence-view/v1/buddylist", nullptr, ct);
    } catch (const ApiError& e) {
        if (e.status == 429 || e.status == 401) throw;
        ST_LOG_INFO("spotify", "buddylist on guc-spclient failed ({}), trying spclient.wg", e.status);
        j = spclient("GET", "https://spclient.wg.spotify.com/presence-view/v1/buddylist", nullptr, ct);
    }
    if (!j.is_object()) throw ApiError(0, "Spotify buddylist: invalid JSON");
    return parseBuddylist(j);
}

std::vector<FriendActivity> Api::parseBuddylist(const json& j) {
    std::vector<FriendActivity> out;
    const json& friends = at(j, "friends");
    if (!friends.is_array()) return out;
    for (const auto& f : friends) {
        const json& user = at(f, "user");
        const json& track = at(f, "track");
        FriendActivity a;
        a.userUri = str(user, "uri");
        a.userName = str(user, "name");
        if (a.userName.empty() && a.userUri.rfind("spotify:user:", 0) == 0) a.userName = a.userUri.substr(13);
        a.imageUrl = httpsUrl(str(user, "imageUrl"));
        a.track.id = str(track, "uri");
        a.track.name = str(track, "name");
        if (a.userUri.empty() || a.track.id.empty() || a.track.name.empty()) continue;
        const json& artist = at(track, "artist");
        if (!str(artist, "name").empty()) a.track.artists.push_back({str(artist, "uri"), str(artist, "name")});
        const json& album = at(track, "album");
        a.track.album.id = str(album, "uri");
        a.track.album.name = str(album, "name");
        if (const std::string img = httpsUrl(str(track, "imageUrl")); !img.empty())
            a.track.album.images.push_back({img, 640, 640});   // the buddylist hands out the 640 px cover
        const json& context = at(track, "context");
        a.contextUri = normalizeContextUri(str(context, "uri"));
        a.contextName = str(context, "name");
        const json& ts = at(f, "timestamp");
        if (ts.is_number_integer()) a.timestampMs = ts.get<int64_t>();
        else if (ts.is_number()) a.timestampMs = static_cast<int64_t>(ts.get<double>());
        out.push_back(std::move(a));
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const FriendActivity& x, const FriendActivity& y) { return x.timestampMs > y.timestampMs; });
    return out;
}

// ---- New releases -------------------------------------------------------------------------------------------------
// queryWhatsNewFeed, variables as the web player's What's New panel sends them (dwp-whats-new-feed chunk):
// {offset, limit, onlyUnPlayedItems, includedContentTypes:["ALBUM"] (the "Music" filter), includeEpisodeContentRatingsV2}.
// Response: data.whatsNewFeedItems.items[] of {id, state:{state:"NEW"|"SEEN"}, content:{__typename:
// "AlbumResponseWrapper"|"EpisodeOrChapterResponseWrapper", data:{__typename:"Album", uri, name, date:{isoString},
// artists:{items:[{uri, profile:{name}}]}, coverArt:{sources}, ...}}}.
std::vector<Album> Api::whatsNewReleases(int limit, const CT& ct) {
    json vars{{"offset", 0},
              {"limit", limit},
              {"onlyUnPlayedItems", false},
              {"includedContentTypes", json::array({"ALBUM"})},
              {"includeEpisodeContentRatingsV2", false}};
    json data;
    try {
        data = query("queryWhatsNewFeed", vars, ct);
    } catch (const ApiError& e) {
        // A document that no longer takes the content filter: ask for everything (the parser keeps the albums).
        if (e.status != 400) throw;
        vars["includedContentTypes"] = json::array();
        data = query("queryWhatsNewFeed", vars, ct);
    }
    return parseWhatsNewFeed(data);
}

std::vector<Album> Api::parseWhatsNewFeed(const json& data) {
    std::vector<Album> out;
    std::unordered_set<std::string> seen;
    const json& items = at(at(data, "whatsNewFeedItems"), "items");
    if (!items.is_array()) return out;
    for (const auto& it : items) {
        const json& content = at(it, "content");
        const json& d = at(content, "data");
        const std::string wrapper = str(content, "__typename");
        const std::string tn = str(d, "__typename");
        if (tn != "Album" && !(tn.empty() && wrapper == "AlbumResponseWrapper")) continue;   // episodes, chapters...
        Album a = releaseFrom(d);
        if (a.id.rfind("spotify:album:", 0) != 0 || a.name.empty()) continue;
        if (seen.insert(a.id).second) out.push_back(std::move(a));
    }
    return out;
}

std::vector<Album> Api::artistLatestReleases(const std::string& artistUri, const CT& ct) {
    const json vars{{"uri", artistUri}, {"locale", ""}, {"preReleaseV2", false}};
    return parseArtistReleases(query("queryArtistOverview", vars, ct));
}

// data.artistUnion.discography: latest {uri, name, type, date:{year, month, day, precision}, coverArt, tracks} and
// albums / singles {items:[{releases:{items:[release]}}]} (newest first).
std::vector<Album> Api::parseArtistReleases(const json& data) {
    const json& a = at(data, "artistUnion");
    const json& disc = at(a, "discography");
    const ArtistRef artist{str(a, "uri"), str(at(a, "profile"), "name")};
    std::vector<Album> out;
    std::unordered_set<std::string> seen;
    auto add = [&](const json& d) {
        if (!d.is_object()) return;
        Album al = releaseFrom(d);
        if (al.id.rfind("spotify:album:", 0) != 0 || al.name.empty() || !seen.insert(al.id).second) return;
        if (al.artists.empty() && !artist.name.empty()) al.artists.push_back(artist);
        out.push_back(std::move(al));
    };
    add(at(disc, "latest"));
    for (const char* group : {"albums", "singles"}) {
        const json& items = at(at(disc, group), "items");
        if (!items.is_array() || items.empty()) continue;
        const json& releases = at(at(items[0], "releases"), "items");
        if (releases.is_array() && !releases.empty()) add(releases[0]);
        else add(items[0]);   // a flat list of releases
    }
    return out;
}

} // namespace st::spotify
