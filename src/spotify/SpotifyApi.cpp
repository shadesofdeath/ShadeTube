#include "spotify/SpotifyApi.h"

#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "spotify/Auth.h" // kUserAgent
#include "spotify/HashRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace st::spotify {
namespace {

using json = nlohmann::json;

constexpr const char* kEndpoint = "https://api-partner.spotify.com/pathfinder/v2/query";
// The web player's playlist service (create / rename / rootlist edits). Same Bearer token as Pathfinder.
constexpr const char* kPlaylistService = "https://spclient.wg.spotify.com/playlist/v2";

// Persisted-query hashes (per operation name) come from spotify/HashRegistry: a built-in table plus hashes learned
// from the live web player when Spotify retires one (self-healing, see query()).

// ---- small JSON helpers (never throw on unexpected shapes) --------------------------------------------------
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
    const json* items = nullptr;
    if (artists.is_object() && at(artists, "items").is_array()) items = &at(artists, "items");
    else if (artists.is_array()) items = &artists;
    if (!items) return out;
    for (const auto& a : *items) {
        const json& d = at(a, "data").is_object() ? at(a, "data") : a;
        ArtistRef r;
        r.id = str(d, "uri");
        r.name = str(at(d, "profile"), "name");
        if (r.name.empty()) r.name = str(d, "name");
        if (!r.name.empty()) out.push_back(std::move(r));
    }
    return out;
}

std::string dateFrom(const json& date) {
    if (!date.is_object()) return {};
    const std::string iso = str(date, "isoString");
    if (iso.size() >= 10) return iso.substr(0, 10);
    if (at(date, "year").is_number()) return std::to_string(at(date, "year").get<int>());
    return {};
}

Track trackFrom(const json& d, const AlbumRef* albumCtx = nullptr) {
    Track t;
    t.id = str(d, "uri");
    t.name = str(d, "name");
    if (at(d, "trackDuration").is_object()) t.durationMs = integer(at(d, "trackDuration"), "totalMilliseconds");
    else if (at(d, "duration").is_object()) t.durationMs = integer(at(d, "duration"), "totalMilliseconds");
    t.trackNumber = integer(d, "trackNumber");
    if (at(d, "playability").is_object()) {
        const json& p = at(at(d, "playability"), "playable");
        t.playable = p.is_boolean() ? p.get<bool>() : true;
    }
    t.explicitContent = str(at(d, "contentRating"), "label") == "EXPLICIT";
    if (at(d, "playcount").is_string()) {
        try {
            t.listenCount = std::stoll(at(d, "playcount").get<std::string>());
        } catch (...) {
        }
    }
    t.artists = artistRefsFrom(at(d, "artists"));
    if (at(d, "albumOfTrack").is_object()) {
        const json& a = at(d, "albumOfTrack");
        t.album.id = str(a, "uri");
        t.album.name = str(a, "name");
        t.album.images = imagesFrom(at(at(a, "coverArt"), "sources"));
        if (t.artists.empty()) t.artists = artistRefsFrom(at(a, "artists"));
    } else if (albumCtx) {
        t.album = *albumCtx;
    }
    return t;
}

// ownerV2.data {username, name} + currentUserCapabilities.canEditItems (libraryV3 items and fetchPlaylist).
// `me` = the logged-in username (may be empty before me() ran: then nothing counts as owned).
void ownershipFrom(const json& d, const std::string& me, Playlist& p) {
    const json& owner = at(at(d, "ownerV2"), "data");
    p.ownerId = str(owner, "username");
    if (p.ownerId.empty()) p.ownerId = str(owner, "id");
    if (p.ownerId.empty()) {   // spotify:user:<name>
        const std::string ou = str(owner, "uri");
        if (ou.rfind("spotify:user:", 0) == 0) p.ownerId = ou.substr(13);
    }
    p.ownerName = str(owner, "name");
    const json& canEdit = at(at(d, "currentUserCapabilities"), "canEditItems");
    p.owned = !me.empty() && p.ownerId == me;
    p.editable = canEdit.is_boolean() ? canEdit.get<bool>() : p.owned;
    // "Your Episodes" is a real playlist (format "listen-later") the user owns, but it's the podcast queue: not a
    // music playlist to add songs to, rename or delete.
    if (str(d, "format") == "listen-later") p.owned = p.editable = false;
}

Playlist playlistFrom(const json& d, const std::string& me = {}) {
    Playlist p;
    p.id = str(d, "uri");
    p.name = str(d, "name");
    p.description = str(d, "description");
    const json& items = at(at(d, "images"), "items");
    if (items.is_array() && !items.empty()) p.images = imagesFrom(at(items[0], "sources"));
    ownershipFrom(d, me, p);
    return p;
}

// "2022-06-14T16:13:04.660Z" -> unix seconds (0 for the epoch placeholder / garbage).
int64_t isoToUnix(const std::string& iso) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (iso.size() < 19 || sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
    if (y <= 1970 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
    const auto days = std::chrono::sys_days(std::chrono::year(y) / std::chrono::month(static_cast<unsigned>(mo)) /
                                            std::chrono::day(static_cast<unsigned>(d)));
    return days.time_since_epoch().count() * 86400LL + h * 3600LL + mi * 60LL + se;
}

std::string lastSegment(const std::string& uri) {
    const size_t p = uri.rfind(':');
    return p == std::string::npos ? uri : uri.substr(p + 1);
}

Artist artistFrom(const json& d) {
    Artist ar;
    ar.id = str(d, "uri");
    ar.name = str(at(d, "profile"), "name");
    if (ar.name.empty()) ar.name = str(d, "name");
    const json& avatar = at(at(d, "visuals"), "avatarImage");
    if (avatar.is_object()) ar.images = imagesFrom(at(avatar, "sources"));
    return ar;
}

Album albumFrom(const json& a, bool withTracks) {
    Album al;
    al.id = str(a, "uri");
    al.name = str(a, "name");
    al.primaryType = str(a, "type");
    al.label = str(a, "label");
    al.firstReleaseDate = dateFrom(at(a, "date"));
    al.images = imagesFrom(at(at(a, "coverArt"), "sources"));
    al.artists = artistRefsFrom(at(a, "artists"));
    if (withTracks && at(a, "tracksV2").is_object()) {
        al.totalTracks = integer(at(a, "tracksV2"), "totalCount");
        AlbumRef ctx{al.id, al.name, al.images};
        const json& items = at(at(a, "tracksV2"), "items");
        if (items.is_array())
            for (const auto& it : items)
                if (at(it, "track").is_object()) {
                    Track t = trackFrom(at(it, "track"), &ctx);
                    if (t.artists.empty()) t.artists = al.artists;
                    al.tracks.push_back(std::move(t));
                }
    }
    if (al.totalTracks == 0) al.totalTracks = static_cast<int>(al.tracks.size());
    return al;
}

std::string plainText(const std::string& html);   // below, with the home-feed parser

} // namespace

void Api::setCredentials(std::string accessToken, std::string spDc) {
    std::lock_guard lock(mutex_);
    accessToken_ = std::move(accessToken);
    spDc_ = std::move(spDc);
}

bool Api::hasCredentials() const {
    std::lock_guard lock(mutex_);
    return !accessToken_.empty();
}

// Self-healing: when Pathfinder rejects the op's hash (HTTP 412 "Invalid query hash": Spotify purged that document
// version), HashRegistry::heal() learns the web player's current hash (throttled, see HashRegistry.h) and the
// request is retried ONCE with it. A 400 that names a variable is a request problem, never a hash refresh.
json Api::query(const std::string& operationName, const json& variables, const CT& ct, const char* acceptLanguage) {
    std::string token, cookie;
    {
        std::lock_guard lock(mutex_);
        token = accessToken_;
        cookie = spDc_;
    }
    if (token.empty()) throw ApiError(401, "Spotify: not authenticated");
    auto& registry = HashRegistry::instance();
    std::string hash = registry.hash(operationName);
    if (hash.empty()) throw ApiError(0, "Spotify " + operationName + ": no persisted-query hash known");

    for (int attempt = 0;; ++attempt) {
        json body = {
            {"variables", variables},
            {"operationName", operationName},
            {"extensions", {{"persistedQuery", {{"version", 1}, {"sha256Hash", hash}}}}},
        };

        http::HttpRequest r;
        r.method = "POST";
        r.url = kEndpoint;
        r.headers = {
            {"Authorization", "Bearer " + token},
            {"Content-Type", "application/json"},
            {"Accept", "application/json"},
            {"User-Agent", kUserAgent},
            {"App-Platform", "WebPlayer"},
            {"Cookie", "sp_dc=" + cookie + ";"},
        };
        if (acceptLanguage && *acceptLanguage) r.headers.push_back({"Accept-Language", acceptLanguage});
        r.body = body.dump();

        const auto resp = http::send(r, ct);
        auto j = resp.isSuccessStatusCode() ? json::parse(resp.body, nullptr, false) : json();
        const bool gqlErrors = at(j, "errors").is_array() && !at(j, "errors").empty();
        if (resp.isSuccessStatusCode() && !gqlErrors) {
            if (j.is_discarded()) throw ApiError(resp.statusCode, "Spotify " + operationName + ": invalid JSON");
            if (!at(j, "data").is_object()) throw ApiError(resp.statusCode, "Spotify " + operationName + ": no data");
            if (attempt > 0) ST_LOG_INFO("spotify", "{} succeeded with the refreshed hash", operationName);
            return j["data"];
        }

        // Classify the failure (for a 2xx, only its GraphQL errors: data may contain '$' or "variable" anywhere).
        const std::string detail = gqlErrors ? at(j, "errors").dump() : resp.body;
        const std::string shortDetail = detail.substr(0, std::min<size_t>(detail.size(), 700));
        std::string variable;
        const QueryFailure kind = classifyFailure(resp.statusCode, detail, &variable);
        if (kind == QueryFailure::HashRejected && attempt == 0) {
            ST_LOG_WARN("spotify", "{} HTTP {}: persisted-query hash {}... rejected ({})", operationName, resp.statusCode,
                        hash.substr(0, 8), shortDetail);
            const std::string fresh = registry.heal(operationName, hash, ct);   // blocking (worker thread)
            if (!fresh.empty() && fresh != hash) {
                ST_LOG_INFO("spotify", "{}: retrying with hash {}... (was {}...)", operationName, fresh.substr(0, 8),
                            hash.substr(0, 8));
                hash = fresh;
                continue;
            }
        } else if (kind == QueryFailure::VariableError) {
            ST_LOG_WARN("spotify",
                        "{} HTTP {}: the query document (hash {}...) rejects our variables{}{}: the request needs "
                        "updating, not the hash. vars={} body={}",
                        operationName, resp.statusCode, hash.substr(0, 8), variable.empty() ? "" : ": $", variable,
                        variables.dump(), shortDetail);
        } else if (kind != QueryFailure::HashRejected) {
            ST_LOG_WARN("spotify", "{} HTTP {} vars={} body={}", operationName, resp.statusCode, variables.dump(),
                        shortDetail);
        } else if (attempt > 0) {
            ST_LOG_WARN("spotify", "{}: the refreshed hash {}... was rejected too (HTTP {})", operationName,
                        hash.substr(0, 8), resp.statusCode);
        }
        if (gqlErrors)
            throw ApiError(resp.statusCode, "Spotify " + operationName + ": " + str(at(j, "errors")[0], "message"));
        throw ApiError(resp.statusCode, "Spotify " + operationName + " HTTP " + std::to_string(resp.statusCode));
    }
}

// ---- Mutations (Pathfinder) -----------------------------------------------------------------------------
// The web-player token only authorizes api-partner (Pathfinder), NOT api.spotify.com/v1 (which 429s). So the
// library/playlist writes go through the internal addToLibrary/removeFromLibrary/add*Playlist mutations.
namespace {
std::string toUri(const char* kind, const std::string& idOrUri) {
    return idOrUri.rfind("spotify:", 0) == 0 ? idOrUri : std::string("spotify:") + kind + ":" + idOrUri;
}
std::vector<std::string> toUris(const char* kind, const std::vector<std::string>& in) {
    std::vector<std::string> out;
    out.reserve(in.size());
    for (const auto& s : in) out.push_back(toUri(kind, s));
    return out;
}
} // namespace

bool Api::setTracksSaved(const std::vector<std::string>& ids, bool saved, const CT& ct) {
    if (ids.empty()) return true;
    try {
        query(saved ? "addToLibrary" : "removeFromLibrary", json{{"libraryItemUris", toUris("track", ids)}}, ct);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "setTracksSaved failed: {}", e.what());
        return false;
    }
}
bool Api::setAlbumsSaved(const std::vector<std::string>& ids, bool saved, const CT& ct) {
    if (ids.empty()) return true;
    try {
        query(saved ? "addToLibrary" : "removeFromLibrary", json{{"libraryItemUris", toUris("album", ids)}}, ct);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "setAlbumsSaved failed: {}", e.what());
        return false;
    }
}
bool Api::setArtistsFollowed(const std::vector<std::string>& ids, bool follow, const CT& ct) {
    if (ids.empty()) return true;
    try {
        query(follow ? "addToLibrary" : "removeFromLibrary", json{{"libraryItemUris", toUris("artist", ids)}}, ct);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "setArtistsFollowed failed: {}", e.what());
        return false;
    }
}
// ---- Playlist items (Pathfinder; web-player bundle web-player.da8b87c8.js, class pj) ------------------------
//   add:    {playlistUri, playlistItemUris:[uri...], newPosition:{moveType, fromUid}}
//   remove: {playlistUri, uids:[uid...]}       (row uids from fetchPlaylist content.items[].uid)
bool Api::addToPlaylist(const std::string& playlistUri, const std::vector<std::string>& trackUris, const CT& ct) {
    if (trackUris.empty()) return true;
    const json vars{{"playlistUri", toUri("playlist", playlistUri)},
                    {"playlistItemUris", toUris("track", trackUris)},
                    {"newPosition", {{"moveType", "BOTTOM_OF_PLAYLIST"}, {"fromUid", nullptr}}}};
    try {
        query("addToPlaylist", vars, ct);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "addToPlaylist failed: {}", e.what());
        return false;
    }
}
bool Api::removeFromPlaylist(const std::string& playlistUri, const std::vector<std::string>& uids, const CT& ct) {
    std::vector<std::string> clean;
    for (const auto& u : uids)
        if (!u.empty()) clean.push_back(u);
    if (clean.empty()) return uids.empty();
    const json vars{{"playlistUri", toUri("playlist", playlistUri)}, {"uids", clean}};
    try {
        query("removeFromPlaylist", vars, ct);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "removeFromPlaylist failed: {}", e.what());
        return false;
    }
}

// ---- Playlist service (spclient /playlist/v2) — what the web player's RootlistAPI / playlist editor call ---------
// Bodies are the JSON form of Spotify's playlist4 protos (ts-proto toJSON: enums as strings):
//   create:   POST /playlist                 Delta{ops:[UPDATE_LIST_ATTRIBUTES{name}], info}  -> {uri, revision}
//   rename:   POST /playlist/<id>/changes    ListChanges{deltas:[Delta]}
//   rootlist: GET /user/<u>/rootlist?decorate=...  and  POST /user/<u>/rootlist/changes  ListChanges (ADD / REM)
json Api::playlistService(const char* method, const std::string& path, const json* body, const CT& ct) {
    return spclient(method, std::string(kPlaylistService) + path, body, ct);
}

json Api::spclient(const char* method, const std::string& url, const json* body, const CT& ct, int quietStatus) {
    std::string token, cookie;
    {
        std::lock_guard lock(mutex_);
        token = accessToken_;
        cookie = spDc_;
    }
    if (token.empty()) throw ApiError(401, "Spotify: not authenticated");

    http::HttpRequest r;
    r.method = method;
    r.url = url;
    r.headers = {
        {"Authorization", "Bearer " + token},
        {"Accept", "application/json"},
        {"User-Agent", kUserAgent},
        {"App-Platform", "WebPlayer"},
        {"Origin", "https://open.spotify.com"},
        {"Referer", "https://open.spotify.com/"},
        {"Cookie", "sp_dc=" + cookie + ";"},
    };
    if (body) {
        r.headers.push_back({"Content-Type", "application/json;charset=UTF-8"});
        r.body = body->dump();
    }
    const auto resp = http::send(r, ct);
    const std::string host = "https://spclient.wg.spotify.com";
    const std::string path = url.rfind(host, 0) == 0 ? url.substr(host.size()) : url;
    const std::string where = std::string(method) + " " + path.substr(0, path.find('?'));
    if (!resp.isSuccessStatusCode()) {
        if (resp.statusCode != quietStatus)
            ST_LOG_WARN("spotify", "spclient {} HTTP {} body={}", where, resp.statusCode,
                        resp.body.substr(0, std::min<size_t>(resp.body.size(), 500)));
        throw ApiError(resp.statusCode, "Spotify " + where + " HTTP " + std::to_string(resp.statusCode));
    }
    if (resp.body.empty()) return json();
    auto j = json::parse(resp.body, nullptr, false);
    return j.is_discarded() ? json() : j;
}

// The server parses these strictly: send exactly what the web player's ts-proto toJSON emits — defaults (false,
// empty lists) omitted, and no field the message doesn't have (the create body is `Ops`, which has no `info`).
namespace {
json webPlayerInfo() { return json{{"source", {{"client", "WEBPLAYER"}}}}; }
json listChanges(json ops) {   // ListChanges{deltas:[Delta{ops, info}]}
    return json{{"deltas", json::array({json{{"ops", std::move(ops)}, {"info", webPlayerInfo()}}})}};
}
json nameOp(const std::string& name, const std::string& description) {
    json values{{"name", name}};
    if (!description.empty()) values["description"] = description;
    return json{{"kind", "UPDATE_LIST_ATTRIBUTES"}, {"updateListAttributes", {{"newAttributes", {{"values", std::move(values)}}}}}};
}
int64_t unixMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

std::string Api::username() const {
    std::lock_guard lock(mutex_);
    return username_;
}
void Api::setUsername(std::string username) {
    std::lock_guard lock(mutex_);
    username_ = std::move(username);
}

json Api::rootlist(const CT& ct) {
    std::string user = username();
    if (user.empty()) user = me(ct).username;
    if (user.empty()) throw ApiError(0, "Spotify: unknown username");
    const std::string path = "/user/" + http::urlEncode(user) +
                             "/rootlist?decorate=revision%2Clength%2Cattributes%2Ctimestamp%2Cowner%2Ccapabilities"
                             "&bustCache=" +
                             std::to_string(unixMs());
    return playlistService("GET", path, nullptr, ct);
}

void Api::rootlistChanges(const json& ops, const CT& ct) {
    std::string user = username();
    if (user.empty()) user = me(ct).username;
    if (user.empty()) throw ApiError(0, "Spotify: unknown username");
    const json body = listChanges(ops);
    playlistService("POST", "/user/" + http::urlEncode(user) + "/rootlist/changes", &body, ct);
}

std::vector<std::string> Api::rootlistPlaylists(const CT& ct) {
    const json root = rootlist(ct);
    std::vector<std::string> out;
    const json& items = at(at(root, "contents"), "items");
    if (items.is_array())
        for (const auto& it : items) {
            const std::string uri = str(it, "uri");
            if (uri.rfind("spotify:playlist:", 0) == 0) out.push_back(uri);
        }
    return out;
}

std::string Api::createPlaylist(const std::string& name, const std::string& description, const CT& ct) {
    if (name.empty()) return {};
    std::string uri;
    try {
        const json body{{"ops", json::array({nameOp(name, description)})}};   // Ops{ops} (no info)
        const json created = playlistService("POST", "/playlist", &body, ct);
        uri = str(created, "uri");
        if (uri.rfind("spotify:playlist:", 0) != 0) {
            ST_LOG_WARN("spotify", "createPlaylist: no playlist uri in the response");
            return {};
        }
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "createPlaylist failed: {}", e.what());
        return {};
    }
    // A new playlist exists on its own; the web player then adds it to the top of the user's rootlist (library).
    try {
        const json item{{"uri", uri}, {"attributes", {{"timestamp", std::to_string(unixMs())}}}};
        const json ops = json::array(
            {json{{"kind", "ADD"}, {"add", {{"addFirst", true}, {"items", json::array({item})}}}}});
        rootlistChanges(ops, ct);
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "createPlaylist: adding {} to the library failed: {}", uri, e.what());
        return {};
    }
    ST_LOG_INFO("spotify", "created playlist {}", uri);
    return uri;
}

bool Api::renamePlaylist(const std::string& playlistUri, const std::string& name, const CT& ct) {
    if (name.empty() || playlistUri.empty()) return false;
    try {
        const json body = listChanges(json::array({nameOp(name, {})}));
        playlistService("POST", "/playlist/" + lastSegment(playlistUri) + "/changes", &body, ct);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "renamePlaylist failed: {}", e.what());
        return false;
    }
}

bool Api::deletePlaylist(const std::string& playlistUri, const CT& ct) {
    if (playlistUri.rfind("spotify:playlist:", 0) != 0) return false;
    try {
        const json rem{{"itemsAsKey", true}, {"items", json::array({json{{"uri", playlistUri}}})}};
        const json ops = json::array({json{{"kind", "REM"}, {"rem", rem}}});
        rootlistChanges(ops, ct);
        ST_LOG_INFO("spotify", "removed playlist {} from the library", playlistUri);
        return true;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "deletePlaylist failed: {}", e.what());
        return false;
    }
}
std::vector<bool> Api::tracksSaved(const std::vector<std::string>& ids, const CT& ct) {
    std::vector<bool> out(ids.size(), false);
    if (ids.empty()) return out;
    try {
        const json data = query("areEntitiesInLibrary", json{{"uris", toUris("track", ids)}}, ct);
        // Response shape: data.lookup[] of { _uri, data: { saved: bool } } (parsed defensively).
        const json& lookup = at(data, "lookup");
        if (lookup.is_array())
            for (size_t i = 0; i < ids.size() && i < lookup.size(); ++i) {
                const json& e = lookup[i];
                const json& sv = at(at(e, "data"), "saved");
                if (sv.is_boolean()) out[i] = sv.get<bool>();
                else if (at(e, "saved").is_boolean()) out[i] = at(e, "saved").get<bool>();
            }
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "tracksSaved failed: {}", e.what());
    }
    return out;
}

UserProfile Api::me(const CT& ct) {
    const json data = query("profileAttributes", json::object(), ct);
    const json& p = at(at(data, "me"), "profile");
    UserProfile u;
    u.id = str(p, "uri");
    u.name = str(p, "name");
    u.username = str(p, "username");
    const json& src = at(at(p, "avatar"), "sources");
    if (src.is_array() && !src.empty()) u.imageUrl = str(src.back(), "url"); // largest last
    if (!u.username.empty()) setUsername(u.username);
    return u;
}

namespace {
// `flatten`: playlists inside folders are listed too (folder rows themselves are skipped by the parser).
json libraryVariables(const char* filter, int offset = 0, bool flatten = false) {
    return json{
        {"order", nullptr},
        {"textFilter", ""},
        {"features", {"LIKED_SONGS", "YOUR_EPISODES_V2", "PRERELEASES", "PRERELEASES_V2", "EVENTS"}},
        {"filters", {filter}},
        {"limit", 50},
        {"offset", offset},
        {"flatten", flatten},
        {"expandedFolders", json::array()},
        {"folderUri", nullptr},
        {"includeFoldersWhenFlattening", true},
    };
}
const json& libraryItems(const json& data) { return at(at(at(data, "me"), "libraryV3"), "items"); }
} // namespace

std::vector<Playlist> Api::libraryPlaylists(const CT& ct) {
    const std::string me = username();
    std::vector<Playlist> out;
    std::unordered_set<std::string> seen;
    int libTotal = 0;   // libraryV3 totalCount (logged: shows whether the pseudo-playlists count toward paging)
    static std::atomic<bool> loggedShape{false};
    // libraryV3 pages 50 at a time; follow totalCount (capped so a pathological library can't loop forever).
    for (int offset = 0, page = 0; page < 20; ++page) {
        json data;
        try {
            data = query("libraryV3", libraryVariables("Playlists", offset, /*flatten=*/true), ct);
        } catch (const std::exception& e) {
            if (page == 0) throw;   // nothing to show; later pages: keep what we have
            ST_LOG_WARN("spotify", "libraryV3 playlists page at {} failed: {}", offset, e.what());
            break;
        }
        const json& lib = at(at(data, "me"), "libraryV3");
        const json& items = at(lib, "items");
        if (!items.is_array() || items.empty()) break;
        for (const auto& entry : items) {
            const json& wrap = at(entry, "item");
            const json& d = at(wrap, "data");
            if (!d.is_object()) continue;
            const std::string tn = str(wrap, "__typename");
            Playlist p;
            if (tn.find("PseudoPlaylist") != std::string::npos || str(d, "__typename") == "PseudoPlaylist") {
                p.id = str(d, "uri");
                // Shown as is in the sidebar / library: the app's name for it in the UI language.
                p.name = p.id == kLikedSongsUri ? toUtf8(tr(L"Beğenilen Şarkılar")) : str(d, "name");
                p.totalTracks = integer(d, "count");
                p.images = imagesFrom(at(at(d, "image"), "sources"));
                p.ownerId = me;   // Liked Songs / Your Episodes: the user's, but not editable as a playlist
            } else {
                if (str(d, "__typename") != "Playlist") continue;   // folders are flattened away; skip unknowns
                p = playlistFrom(d, me);
                p.countKnown = false;   // libraryV3 carries no track count; filled from the rootlist below
                if (!loggedShape.exchange(true)) {
                    const std::string raw = entry.dump();
                    ST_LOG_DEBUG("spotify", "libraryV3 playlist item (keys for counts): {}",
                                 raw.substr(0, std::min<size_t>(raw.size(), 1200)));
                }
            }
            if (!p.id.empty() && seen.insert(p.id).second) out.push_back(std::move(p));
        }
        offset += static_cast<int>(items.size());
        libTotal = integer(lib, "totalCount");
        if (offset >= libTotal) break;
    }

    // Track counts: the rootlist decorated with `length` lists every playlist the user has with its size.
    try {
        const json root = rootlist(ct);
        const json& items = at(at(root, "contents"), "items");
        const json& metas = at(at(root, "contents"), "metaItems");
        std::unordered_map<std::string, int> lengths;
        if (items.is_array() && metas.is_array() && metas.size() == items.size()) {
            for (size_t i = 0; i < items.size(); ++i) {
                const std::string uri = str(items[i], "uri");
                if (uri.rfind("spotify:playlist:", 0) == 0 && metas[i].is_object())
                    lengths[uri] = integer(metas[i], "length");   // proto3 JSON omits a zero length
            }
        }
        for (auto& p : out)
            if (auto it = lengths.find(p.id); it != lengths.end()) {
                p.totalTracks = it->second;
                p.countKnown = true;
            }
        ST_LOG_INFO("spotify", "playlists: {} listed (libraryV3 totalCount {}); rootlist: {} items, {} with a length",
                    out.size(), libTotal, items.is_array() ? items.size() : 0, lengths.size());
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "rootlist (playlist counts) failed: {}", e.what());
    }
    return out;
}

std::vector<Album> Api::libraryAlbums(const CT& ct) {
    const json data = query("libraryV3", libraryVariables("Albums"), ct);
    std::vector<Album> out;
    const json& items = libraryItems(data);
    if (items.is_array())
        for (const auto& entry : items) {
            const json& d = at(at(entry, "item"), "data");
            if (d.is_object() && !str(d, "uri").empty()) out.push_back(albumFrom(d, false));
        }
    return out;
}

std::vector<Artist> Api::libraryArtists(const CT& ct) {
    const json data = query("libraryV3", libraryVariables("Artists"), ct);
    std::vector<Artist> out;
    const json& items = libraryItems(data);
    if (items.is_array())
        for (const auto& entry : items) {
            const json& d = at(at(entry, "item"), "data");
            if (d.is_object() && !str(d, "uri").empty()) out.push_back(artistFrom(d));
        }
    return out;
}

// Liked Songs via the fetchLibraryTracks GraphQL op (the collection URI is not a playlist, so fetchPlaylist
// rejects it). Response lives under data.me.library.tracks.
Page<Track> Api::savedTracks(int offset, int limit, const CT& ct, Playlist* outMeta) {
    const json vars{{"offset", offset}, {"limit", limit}, {"order", nullptr}};
    const json data = query("fetchLibraryTracks", vars, ct);

    Page<Track> page;
    page.offset = offset;
    const json& tracks = at(at(at(data, "me"), "library"), "tracks");
    page.total = integer(tracks, "totalCount");
    const json& items = at(tracks, "items");
    if (items.is_array())
        for (const auto& it : items) {
            const json& tw = at(it, "track");   // { _uri, data:{...Track} }  — uri lives on the wrapper
            const json& d = at(tw, "data");
            if (!d.is_object()) continue;
            Track t = trackFrom(d);
            if (t.id.empty()) t.id = str(tw, "_uri");
            if (t.id.empty()) continue;
            t.addedAt = isoToUnix(str(at(it, "addedAt"), "isoString"));
            page.items.push_back(std::move(t));
        }
    const int raw = items.is_array() ? static_cast<int>(items.size()) : 0;   // skipped rows still take an offset
    page.nextOffset = offset + raw;
    page.hasMore = raw > 0 && page.nextOffset < page.total;
    if (outMeta) {
        outMeta->id = kLikedSongsUri;
        outMeta->name = toUtf8(tr(L"Beğenilen Şarkılar"));
        outMeta->totalTracks = page.total;
    }
    return page;
}

Page<Track> Api::playlistTracks(const std::string& uri, int offset, int limit, const CT& ct, Playlist* outMeta,
                                std::string* outOwner) {
    if (uri == kLikedSongsUri) return savedTracks(offset, limit, ct, outMeta);
    // fetchPlaylist requires enableWatchFeedEntrypoint (Boolean!) — omitting it 400s with VALIDATION_INVALID_TYPE_VARIABLE.
    const json vars{{"uri", uri}, {"offset", offset}, {"limit", limit}, {"enableWatchFeedEntrypoint", false}};
    const json data = query("fetchPlaylist", vars, ct);

    const json& pv2 = at(data, "playlistV2");
    if (outMeta && pv2.is_object()) {
        outMeta->id = str(pv2, "uri").empty() ? uri : str(pv2, "uri");
        outMeta->name = str(pv2, "name");
        outMeta->description = plainText(str(pv2, "description"));   // the header shows text, not HTML
        const json& imgItems = at(at(pv2, "images"), "items");
        if (imgItems.is_array() && !imgItems.empty()) outMeta->images = imagesFrom(at(imgItems[0], "sources"));
        ownershipFrom(pv2, username(), *outMeta);
        outMeta->totalTracks = integer(at(pv2, "content"), "totalCount");
        outMeta->countKnown = at(pv2, "content").is_object();
    }
    if (outOwner) {
        const json& owner = at(at(pv2, "ownerV2"), "data");
        *outOwner = str(owner, "name");
        if (outOwner->empty()) *outOwner = str(at(pv2, "owner"), "name");
    }

    Page<Track> page;
    page.offset = offset;
    const json& content = at(pv2, "content");
    if (content.is_object()) {
        page.total = integer(content, "totalCount");
        const json& items = at(content, "items");
        if (items.is_array()) {
            for (const auto& it : items) {
                const json& d = at(at(it, "itemV2"), "data");
                if (!d.is_object() || str(d, "__typename") != "Track") continue;
                Track t = trackFrom(d);
                t.uid = str(it, "uid");   // the row id removeFromPlaylist needs
                t.addedAt = isoToUnix(str(at(it, "addedAt"), "isoString"));
                page.items.push_back(std::move(t));
            }
            // Skipped rows (episodes, unavailable items) still occupy server offsets: page on from the raw count,
            // or the next page would repeat rows (and a duplicated row uid would be "removed" twice locally).
            const int raw = static_cast<int>(items.size());
            page.nextOffset = offset + raw;
            page.hasMore = raw > 0 && page.nextOffset < page.total;
            return page;
        }
    } else if (at(at(data, "me"), "library").is_object()) {
        // savedTracks-style shape (data.me.library.tracks)
        const json& tracks = at(at(at(data, "me"), "library"), "tracks");
        page.total = integer(tracks, "totalCount");
        const json& items = at(tracks, "items");
        if (items.is_array())
            for (const auto& it : items) {
                const json& d = at(at(it, "track"), "data");
                if (d.is_object()) page.items.push_back(trackFrom(d));
            }
    }
    page.hasMore = offset + static_cast<int>(page.items.size()) < page.total;
    return page;
}

Album Api::album(const std::string& uri, const CT& ct) {
    const json vars{{"uri", uri}, {"offset", 0}, {"limit", 50}};
    const json data = query("getAlbum", vars, ct);
    const json& a = at(data, "albumUnion");
    if (!a.is_object()) throw ApiError(404, "album not found");
    return albumFrom(a, true);
}

SearchResults Api::search(const std::string& q, int limit, const CT& ct) {
    const json vars{
        {"searchTerm", q},         {"offset", 0},
        {"limit", limit},          {"numberOfTopResults", 5},
        {"includeAudiobooks", false}, {"includeArtistHasConcertsField", false},
        {"includePreReleases", false}, {"includeLocalConcertsField", false},
        {"includeAuthors", false},
    };
    const json data = query("searchDesktop", vars, ct);
    const json& s = at(data, "searchV2");
    SearchResults out;

    const json& artists = at(at(s, "artists"), "items");
    if (artists.is_array())
        for (const auto& it : artists)
            if (at(it, "data").is_object()) out.artists.push_back(artistFrom(at(it, "data")));

    const json& albums = at(at(s, "albumsV2"), "items");
    if (albums.is_array())
        for (const auto& it : albums)
            if (at(it, "data").is_object()) out.albums.push_back(albumFrom(at(it, "data"), false));

    const json& tracks = at(at(s, "tracksV2"), "items");
    if (tracks.is_array())
        for (const auto& it : tracks) {
            const json& d = at(at(it, "item"), "data");
            if (d.is_object()) out.tracks.push_back(trackFrom(d));
        }
    return out;
}

ArtistPage Api::artistPage(const std::string& uri, const CT& ct) {
    const json vars{{"uri", uri}, {"locale", ""}, {"preReleaseV2", false}};
    const json data = query("queryArtistOverview", vars, ct);
    const json& a = at(data, "artistUnion");
    ArtistPage page;
    page.artist.id = str(a, "uri");
    page.artist.name = str(at(a, "profile"), "name");
    const json& avatar = at(at(at(a, "visuals"), "avatarImage"), "sources");
    page.artist.images = imagesFrom(avatar);
    if (at(a, "stats").is_object()) page.artist.listenCount = integer(at(a, "stats"), "monthlyListeners");

    // Top tracks
    const json& top = at(at(at(a, "discography"), "topTracks"), "items");
    if (top.is_array())
        for (const auto& it : top) {
            const json& d = at(it, "track").is_object() ? at(it, "track") : at(it, "data");
            if (d.is_object() && !str(d, "uri").empty()) page.topTracks.push_back(trackFrom(d));
        }

    // Albums (popular releases)
    const json& albums = at(at(at(a, "discography"), "popularReleasesAlbums"), "items");
    if (albums.is_array())
        for (const auto& it : albums)
            if (!str(it, "uri").empty()) page.albums.push_back(albumFrom(it, false));

    // Related artists
    const json& related = at(at(at(a, "relatedContent"), "relatedArtists"), "items");
    if (related.is_array())
        for (const auto& it : related) {
            const json& d = at(it, "data").is_object() ? at(it, "data") : it;
            if (!str(d, "uri").empty()) page.similar.push_back(artistFrom(d));
        }
    return page;
}

// ---- Home feed (personalized shelves) -----------------------------------------------------------------------
// Response shape (2026-09): data.home.sectionContainer.sections.items[] of
//   { uri, data:{ __typename:"HomeGenericSectionData"|"HomeRecentlyPlayedSectionData"|"HomeShortsSectionData"...,
//                 title:{transformedLabel}, subtitle:{transformedLabel} },
//     sectionItems:{ totalCount, items:[ { uri, content:{ __typename:"PlaylistResponseWrapper"|..., data:{...} } } ] } }
namespace {

// IANA zone of this PC ("Europe/Istanbul"): Spotify picks the daily/"today" shelves by it. MSVC resolves the
// Windows zone through the OS ICU; fall back to Istanbul (the app's audience) when that is unavailable.
std::string localTimeZone() {
    static const std::string zone = [] {
        try {
            if (const auto* z = std::chrono::current_zone()) return std::string(z->name());
        } catch (...) {
        }
        return std::string("Europe/Istanbul");
    }();
    return zone;
}

void appendUtf8(std::string& out, unsigned long cp) {
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return;
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Playlist descriptions carry HTML (<a href="spotify:artist:...">Name</a>, &amp;, &#x27;). Cards want one plain line.
std::string plainText(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    for (size_t i = 0; i < html.size();) {
        const char ch = html[i];
        if (ch == '<') {
            const size_t end = html.find('>', i);
            if (end != std::string::npos) {
                i = end + 1;
                continue;
            }   // an unmatched '<' ("Duman <3") is literal text, not a tag
        }
        if (ch == '&') {
            const size_t end = html.find(';', i);
            if (end != std::string::npos && end - i >= 2 && end - i <= 10) {
                const std::string ent = html.substr(i + 1, end - i - 1);
                std::string rep;
                if (ent == "amp") rep = "&";
                else if (ent == "quot") rep = "\"";
                else if (ent == "apos") rep = "'";
                else if (ent == "lt") rep = "<";
                else if (ent == "gt") rep = ">";
                else if (ent == "nbsp") rep = " ";
                else if (ent.size() > 1 && ent[0] == '#') {
                    try {
                        const bool hex = ent[1] == 'x' || ent[1] == 'X';
                        const std::string digits = ent.substr(hex ? 2 : 1);
                        size_t used = 0;
                        const unsigned long cp = std::stoul(digits, &used, hex ? 16 : 10);
                        if (used == digits.size()) appendUtf8(rep, cp);
                    } catch (...) {
                    }
                }
                if (!rep.empty()) {
                    out += rep;
                    i = end + 1;
                    continue;
                }
            }
        }
        out.push_back(ch);
        ++i;
    }
    // Collapse runs of whitespace (descriptions contain newlines) and trim.
    std::string clean;
    clean.reserve(out.size());
    bool space = false;
    for (const char ch : out) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            space = !clean.empty();
            continue;
        }
        if (space) clean.push_back(' ');
        space = false;
        clean.push_back(ch);
    }
    return clean;
}

size_t utf8Length(const std::string& s) {
    size_t n = 0;
    for (const char ch : s)
        if ((static_cast<unsigned char>(ch) & 0xC0) != 0x80) ++n;
    return n;
}

// {transformedLabel|text} or a plain string. A template-only base text ("Made For {0}") is useless alone.
std::string labelText(const json& j) {
    if (j.is_string()) return j.get<std::string>();
    std::string s = str(j, "transformedLabel");
    if (s.empty()) s = str(j, "text");
    if (s.empty()) {
        s = str(j, "translatedBaseText");
        if (s.find('{') != std::string::npos) s.clear();
    }
    return s;
}

// Liked Songs shows up in the home feed as an untyped "spotify:user:<name|@>:collection" entry.
bool isLikedSongsUri(const std::string& uri) {
    if (uri == Api::kLikedSongsUri) return true;
    constexpr std::string_view user = "spotify:user:", tail = ":collection";
    return uri.size() > user.size() + tail.size() && uri.rfind(user, 0) == 0 &&
           uri.compare(uri.size() - tail.size(), tail.size(), tail) == 0;
}

std::string artistNames(const std::vector<ArtistRef>& artists, size_t max = 2) {
    std::string s;
    for (size_t i = 0; i < artists.size() && i < max; ++i) s += (i ? ", " : "") + artists[i].name;
    return s;
}

// One section item -> card. Returns false for content the app can't show (podcasts, episodes, shows,
// audiobooks, unavailable/restricted entries, unknown types).
bool homeCard(const json& item, CardItem& out) {
    using Kind = CardItem::Kind;
    const json& content = at(item, "content");
    const json& d = at(content, "data");
    std::string uri = str(item, "uri");
    if (uri.empty()) uri = str(content, "uri");
    std::string tn = d.is_object() ? str(d, "__typename") : std::string{};
    if (tn.empty() && d.is_object()) {   // fall back to the wrapper: "PlaylistResponseWrapper" -> "Playlist"
        tn = str(content, "__typename");
        const size_t w = tn.find("ResponseWrapper");
        tn = w == std::string::npos ? std::string{} : tn.substr(0, w);
    }

    if (isLikedSongsUri(uri) || isLikedSongsUri(str(d, "uri"))) {
        out.kind = Kind::Playlist;
        out.id = Api::kLikedSongsUri;
        out.title = toUtf8(tr(L"Beğenilen Şarkılar"));
        if (d.is_object()) out.images = imagesFrom(at(at(d, "image"), "sources"));
        out.subtitle = toUtf8(tr(L"Çalma listesi"));
        return true;
    }
    if (tn == "Playlist") {
        const Playlist p = playlistFrom(d);
        out.kind = Kind::Playlist;
        out.id = p.id.empty() ? uri : p.id;
        out.title = p.name;
        out.images = p.images;
        out.subtitle = plainText(p.description);
        if (out.subtitle.empty()) out.subtitle = str(at(at(d, "ownerV2"), "data"), "name");
    } else if (tn == "Album") {
        const Album a = albumFrom(d, false);
        out.kind = Kind::Album;
        out.id = a.id.empty() ? uri : a.id;
        out.title = a.name;
        out.images = a.images;
        out.subtitle = artistNames(a.artists);
    } else if (tn == "Artist") {
        const Artist ar = artistFrom(d);
        out.kind = Kind::Artist;
        out.id = ar.id.empty() ? uri : ar.id;
        out.title = ar.name;
        out.images = ar.images;
        out.subtitle = toUtf8(tr(L"Sanatçı"));
    } else if (tn == "Track") {
        Track t = trackFrom(d);
        if (t.id.empty()) t.id = uri;
        out.kind = Kind::Track;
        out.id = t.id;
        out.title = t.name;
        out.images = t.album.images;
        out.subtitle = t.artistLine();
        out.track = std::move(t);
    } else {
        return false;   // Episode, Podcast/Show, Audiobook, NotFound, RestrictedContent, UnknownType...
    }
    return !out.id.empty() && !out.title.empty() && out.id.rfind("spotify:", 0) == 0;
}

} // namespace

json Api::homeData(int sectionItemsLimit, const CT& ct) {
    // Variables exactly as the web player (web-player.da8b87c8.js) sends them: homeEndUserIntegration is a
    // required enum (HomeEndUserIntegration!) — omitting it 400s; sp_t (the anonymous device cookie) may be empty
    // for a logged-in user; facet "" = the default (all) chip; includeEpisodeContentRatingsV2 is a feature flag.
    const json vars{
        {"homeEndUserIntegration", "INTEGRATION_WEB_PLAYER"},
        {"timeZone", localTimeZone()},
        {"sp_t", ""},
        {"facet", ""},
        {"sectionItemsLimit", sectionItemsLimit},
        {"includeEpisodeContentRatingsV2", false},
    };
    // Shelf titles in the UI language ("Berkay İçin Derlendi" / "Made For Berkay"), English as the fallback.
    const std::string lang = toUtf8(i18n::localeName());
    const std::string acceptLanguage = lang + "," + i18n::code() + ";q=0.9,en;q=0.5";
    return query("home", vars, ct, acceptLanguage.c_str());
}

std::vector<Shelf> Api::parseHome(const json& data) {
    std::vector<Shelf> out;
    const json& sections = at(at(at(at(data, "home"), "sectionContainer"), "sections"), "items");
    if (!sections.is_array()) return out;
    for (const auto& s : sections) {
        const json& sd = at(s, "data");
        Shelf shelf;
        shelf.title = labelText(at(sd, "title"));
        // The untitled "shorts" grid only repeats "Jump back in" / "Recently played" items.
        if (shelf.title.empty()) continue;
        const std::string sub = labelText(at(sd, "subtitle"));
        if (utf8Length(sub) <= 26) shelf.label = sub;   // longer blurbs overflow the mono header label
        std::unordered_set<std::string> seen;
        const json& items = at(at(s, "sectionItems"), "items");
        if (items.is_array())
            for (const auto& it : items) {
                CardItem card;
                if (homeCard(it, card) && seen.insert(card.id).second) shelf.items.push_back(std::move(card));
            }
        if (!shelf.items.empty()) out.push_back(std::move(shelf));
    }
    return out;
}

std::vector<Shelf> Api::home(const CT& ct) {
    const json data = homeData(10, ct);
    std::vector<Shelf> shelves = parseHome(data);

    // Once per process: a compact shape summary (section / item typenames) so schema drift is visible in the log.
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        std::string summary;
        const json& sections = at(at(at(at(data, "home"), "sectionContainer"), "sections"), "items");
        if (sections.is_array())
            for (const auto& s : sections) {
                std::map<std::string, int> kinds;
                const json& items = at(at(s, "sectionItems"), "items");
                if (items.is_array())
                    for (const auto& it : items) {
                        const json& c = at(it, "content");
                        std::string k = str(at(c, "data"), "__typename");
                        ++kinds[k.empty() ? str(c, "__typename") : k];
                    }
                summary += "\n  " + str(at(s, "data"), "__typename") + " \"" + labelText(at(at(s, "data"), "title")) + "\":";
                for (const auto& [k, n] : kinds) summary += " " + k + "=" + std::to_string(n);
            }
        ST_LOG_INFO("spotify", "home: {} shelves from {} sections{}", shelves.size(),
                    sections.is_array() ? sections.size() : 0, summary);
        const std::string raw = data.dump();
        ST_LOG_DEBUG("spotify", "home raw (truncated): {}", raw.substr(0, std::min<size_t>(raw.size(), 1500)));
    }
    return shelves;
}

// ---- Radio (spclient inspiredby-mix) ---------------------------------------------------------------------
// The web player's "Go to song radio" / "Go to artist radio": seed_to_playlist returns
// {"mediaItems":[{"uri":"spotify:playlist:37i9dQZF1E..."}]}; the playlist itself is a normal fetchPlaylist.
std::string Api::radioPlaylist(const std::string& seedUri, const CT& ct) {
    // Sent as-is like the web player does (base62 ids and colons only).
    if (seedUri.rfind("spotify:", 0) != 0 ||
        seedUri.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:") != std::string::npos)
        throw ApiError(0, "Spotify radio: invalid seed " + seedUri);
    const std::string url =
        "https://spclient.wg.spotify.com/inspiredby-mix/v2/seed_to_playlist/" + seedUri + "?response-format=json";
    const json j = spclient("GET", url, nullptr, ct);
    for (const auto& m : at(j, "mediaItems")) {
        const std::string uri = str(m, "uri");
        if (uri.rfind("spotify:playlist:", 0) == 0) return uri;
    }
    return {};
}

// ---- Lyrics (spclient color-lyrics) ---------------------------------------------------------------------
// What the web player's lyrics view loads: {"lyrics":{"syncType":"LINE_SYNCED","lines":[{"startTimeMs":"960",
// "words":"...","syllables":[],"endTimeMs":"0"}...],"provider":"MusixMatch",...},"colors":{...}}. 404 = no lyrics.
std::string Api::trackLyrics(const std::string& trackId, const CT& ct) {
    std::string id = trackId;
    if (const auto colon = id.rfind(':'); colon != std::string::npos) id = id.substr(colon + 1);
    if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789") != std::string::npos)
        throw ApiError(0, "Spotify lyrics: invalid track id " + trackId);
    const std::string url = "https://spclient.wg.spotify.com/color-lyrics/v2/track/" + id +
                            "?format=json&vocalRemoval=false&market=from_token";
    try {
        const json j = spclient("GET", url, nullptr, ct, 404);
        return j.is_null() ? std::string() : j.dump();
    } catch (const ApiError& e) {
        if (e.status == 404) return {};
        throw;
    }
}

} // namespace st::spotify
