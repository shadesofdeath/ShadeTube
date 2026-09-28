#include "spotify/HashRegistry.h"

#include "core/Http.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "spotify/Auth.h" // kUserAgent

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace st::spotify {
namespace {

using json = nlohmann::json;

constexpr const char* kWebPlayer = "https://open.spotify.com/";
constexpr const char* kCdnHost = "https://open.spotifycdn.com";
constexpr std::string_view kBuildDir = "/cdn/build/web-player/";
// Last resort (community-maintained, verified daily; lists ~10 common queries, no mutations):
// {schema_version, bundle_id, last_updated, operations:{<op>:{hash, type, status, ...}}}.
constexpr const char* kCommunityRegistry =
    "https://raw.githubusercontent.com/Jigen-Ohtsusuki/spotify-gql-registry/main/hashes.json";
constexpr size_t kMaxRegistryBytes = 1 << 20;             // the real file is ~3 KB
constexpr std::chrono::seconds kChunkBudget{45};          // heal() holds healMutex_ for the whole scan

// The hashes this build was verified with (web-player.da8b87c8.js, 2026-09-27/28). Ops that share a GraphQL
// document share a hash: add/removeFromLibrary take ANY uri (track = save, album = save, artist = follow);
// add/removeFromPlaylist share one. queryArtistOverview / searchDesktop are older documents than the current web
// player's (9f8134ef... / 11483936...) that Spotify still serves; they heal to the current ones once purged.
const std::vector<std::pair<const char*, const char*>> kBuiltins = {
    {"profileAttributes", "08ffb4730af3746e04a8301396f20875dbbce10c75243803091a9274eacc8ac0"},
    {"libraryV3", "390c78e5b951029bad359785e69b07b536a509c581cbcd0aded5e5067f187455"},
    {"fetchPlaylist", "243c0ba2736f16da721e3a227004bbcdb8df6c846f198bd478172e00aa1faf42"},
    {"fetchLibraryTracks", "087278b20b743578a6262c2b0b4bcd20d879c503cc359a2285baf083ef944240"},
    {"getAlbum", "6a74b456cd1735c9193d9e8ec8cc5184cad7ce13572210315229db3975964361"},
    {"queryArtistOverview", "7f86ff63e38c24973a2842b672abe44c910c1973978dc8a4a0cb648edef34527"},
    {"searchDesktop", "d9f785900f0710b31c07818d617f4f7600c1e21217e80f5b043d1e78d74e6026"},
    {"home", "76243c78b0e20ecdbe41b794dec8cbe73f75e585b0a7201b8d2e84578412847a"},
    {"areEntitiesInLibrary", "134337999233cc6fdd6b1e6dbf94841409f04a946c5c7b744b09ba0dfe5a85ed"},
    {"addToLibrary", "1ad0d40b3c09660d818b9e770eb1e84745dfbe941df159a64f8772b6fa2bfc3a"},
    {"removeFromLibrary", "1ad0d40b3c09660d818b9e770eb1e84745dfbe941df159a64f8772b6fa2bfc3a"},
    {"addToPlaylist", "47b2a1234b17748d332dd0431534f22450e9ecbb3d5ddcdacbd83368636a0990"},
    {"removeFromPlaylist", "47b2a1234b17748d332dd0431534f22450e9ecbb3d5ddcdacbd83368636a0990"},
};

bool isHex64(std::string_view s) {
    return s.size() == 64 &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool isIdentChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}
bool isFileChar(char c) { return isIdentChar(c) || c == '.' || c == '-' || c == '~'; }
char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }
bool containsNoCase(std::string_view hay, std::string_view needle) {
    return std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                       [](char a, char b) { return lower(a) == lower(b); }) != hay.end();
}
std::string prefix(const std::string& hash) { return hash.empty() ? std::string("(none)") : hash.substr(0, 8) + "..."; }
int64_t unixNow() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// `{4406:"xpui-routes-search","a1":"x",...}` (the braces stripped) -> pairs. Empty on anything unexpected.
std::vector<std::pair<std::string, std::string>> parseFlatMap(std::string_view s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t i = 0;
    auto quoted = [&](std::string& v) {
        if (i >= s.size() || s[i] != '"') return false;
        const size_t end = s.find('"', i + 1);
        if (end == std::string_view::npos) return false;
        v.assign(s.substr(i + 1, end - i - 1));
        i = end + 1;
        return true;
    };
    while (i < s.size()) {
        std::string key, value;
        if (s[i] == '"') {
            if (!quoted(key)) return {};
        } else {
            const size_t k = i;
            while (i < s.size() && isIdentChar(s[i])) ++i;
            key.assign(s.substr(k, i - k));
        }
        if (key.empty() || i >= s.size() || s[i++] != ':' || !quoted(value)) return {};
        out.emplace_back(std::move(key), std::move(value));
        if (i < s.size() && s[i++] != ',') return {};
    }
    return out;
}

// The server answered with a non-2xx status (it was reached).
struct StatusError : std::runtime_error {
    int status;
    StatusError(int s, const std::string& url) : std::runtime_error("HTTP " + std::to_string(s) + " for " + url), status(s) {}
};
// The request never got an answer (DNS, connect / receive timeout, network down).
struct TransportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

std::string fetchText(const std::string& url, const CT& ct) {
    http::HttpResponse resp;
    try {
        resp = http::get(url, {{"User-Agent", kUserAgent}, {"Accept", "*/*"}}, ct);
    } catch (const std::exception& e) {
        if (ct.isCancellationRequested()) throw;
        throw TransportError(std::string(e.what()) + " (" + url + ")");
    }
    if (!resp.isSuccessStatusCode()) throw StatusError(resp.statusCode, url);
    return resp.body;
}

// Code is only ever fetched from Spotify's own hosts, whatever the page or the bundle's chunk map point at.
bool allowedHost(std::string_view url) {
    return url.starts_with("https://open.spotifycdn.com/") || url.starts_with("https://open.spotify.com/");
}

std::string fileName(const std::string& url) { return url.substr(url.rfind('/') + 1); }

// camelCase words of an op name, lowercased, 4+ letters: "searchDesktop" -> {"search", "desktop"}.
std::vector<std::string> nameWords(const std::string& op) {
    std::vector<std::string> words;
    std::string w;
    auto flush = [&] {
        if (w.size() >= 4) words.push_back(w);
        w.clear();
    };
    for (const char c : op) {
        if (c >= 'A' && c <= 'Z') flush();
        if (isIdentChar(c)) w.push_back(lower(c));
        else flush();
    }
    flush();
    return words;
}

// How likely chunk `name` defines one of the ops `words` came from. Per name segment ("xpui-routes-search"):
// equal to a word +3, overlapping one +1, unrelated -1 (a specific name beats a broad one: "desktop" alone must not
// pull in xpui-desktop-modals), "routes" +1 (route pages own their queries). Numbered chunks come last.
int chunkScore(const std::string& name, const std::vector<std::string>& words) {
    if (name.find_first_not_of("0123456789") == std::string::npos) return -1000;
    int score = 0;
    size_t i = 0;
    while (i < name.size()) {
        size_t e = name.find_first_of("-~_.", i);
        if (e == std::string::npos) e = name.size();
        const std::string seg = name.substr(i, e - i);
        i = e + 1;
        if (seg.empty() || seg == "xpui" || seg == "dwp") continue;
        if (seg == "routes") {
            ++score;
            continue;
        }
        int best = -1;
        for (const auto& w : words) {
            if (seg == w) best = 3;
            else if (best < 1 && seg.size() >= 4 && (seg.find(w) != std::string::npos || w.find(seg) != std::string::npos))
                best = 1;
        }
        score += best;
    }
    return score;
}

} // namespace

// ---- Discovery ---------------------------------------------------------------------------------------------------

std::vector<PersistedOp> extractPersistedOps(std::string_view js) {
    std::vector<std::pair<size_t, PersistedOp>> found;
    for (const std::string_view kind : {std::string_view("query"), std::string_view("mutation")}) {
        const std::string needle = "\",\"" + std::string(kind) + "\",\"";   // name"  ,"query","  hash
        for (size_t p = js.find(needle); p != std::string_view::npos; p = js.find(needle, p + 1)) {
            size_t s = p;
            while (s > 0 && isIdentChar(js[s - 1])) --s;
            if (s == p || s == 0 || js[s - 1] != '"') continue;
            const size_t h = p + needle.size();
            if (h + 65 > js.size() || js[h + 64] != '"') continue;
            const std::string_view hash = js.substr(h, 64);
            if (!isHex64(hash)) continue;
            found.push_back({s, PersistedOp{std::string(js.substr(s, p - s)), std::string(kind), std::string(hash)}});
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<PersistedOp> out;
    std::set<std::string, std::less<>> seen;
    for (auto& [pos, op] : found)
        if (seen.insert(op.name).second) out.push_back(std::move(op));
    return out;
}

std::vector<std::string> findBundleUrls(std::string_view html) {
    std::vector<std::string> out;
    for (size_t p = html.find(kBuildDir); p != std::string_view::npos; p = html.find(kBuildDir, p + 1)) {
        size_t e = p + kBuildDir.size();
        while (e < html.size() && isFileChar(html[e])) ++e;
        const std::string_view file = html.substr(p + kBuildDir.size(), e - p - kBuildDir.size());
        if (!file.starts_with("web-player.") || !file.ends_with(".js")) continue;
        // Keep the page's own origin ("https://open.spotifycdn.com"); a relative src resolves to the CDN. Any other
        // host (or a protocol-relative "//host") is ignored.
        size_t s = p;
        while (s > 0 && html[s - 1] != '"' && html[s - 1] != '\'' && html[s - 1] != '=' && html[s - 1] != ' ' &&
               html[s - 1] != '(')
            --s;
        const std::string_view origin = html.substr(s, p - s);
        std::string url = (origin.empty() ? std::string(kCdnHost) : std::string(origin)) + std::string(kBuildDir) +
                          std::string(file);
        if (!allowedHost(url)) continue;
        if (std::find(out.begin(), out.end(), url) == out.end()) out.push_back(std::move(url));
    }
    return out;
}

std::vector<ChunkRef> findChunkUrls(std::string_view js, std::string_view baseUrl) {
    // Public path: the runtime's absolute u.p="https://.../" (other embedded runtimes use r.p=""), Spotify hosts only.
    std::string base(baseUrl);
    constexpr std::string_view pp = ".p=\"https://";
    for (size_t p = js.find(pp); p != std::string_view::npos; p = js.find(pp, p + 1)) {
        const size_t s = p + 4, e = js.find('"', s);
        if (e != std::string_view::npos && e - s < 200 && js[e - 1] == '/' && allowedHost(js.substr(s, e - s))) {
            base.assign(js.substr(s, e - s));
            break;
        }
    }
    if (!base.empty() && base.back() != '/') base.push_back('/');

    // "<name or id>" + "." + ({id:"hash",...})[e] + ".js"   (the ".css" twin map is skipped)
    constexpr std::string_view join = "+\".\"+({";
    for (size_t p = js.find(join); p != std::string_view::npos; p = js.find(join, p + 1)) {
        const size_t hs = p + join.size();
        const size_t he = js.find('}', hs);
        if (he == std::string_view::npos) break;
        if (js.substr(he, 24).find("+\".js\"") == std::string_view::npos) continue;
        const auto hashes = parseFlatMap(js.substr(hs, he - hs));
        if (hashes.empty()) continue;
        // Optional names map right before: (({1328:"xpui-pip-mini-player",...})[e]||e)
        std::vector<std::pair<std::string, std::string>> names;
        const size_t ne = js.rfind("})[", p);
        if (ne != std::string_view::npos && p - ne < 32 && js.substr(ne, p - ne).find("]||") != std::string_view::npos) {
            const size_t ns = js.rfind("({", ne);
            if (ns != std::string_view::npos) names = parseFlatMap(js.substr(ns + 2, ne - ns - 2));
        }
        std::vector<ChunkRef> out;
        for (const auto& [id, hash] : hashes) {
            std::string name = id;
            for (const auto& [nid, n] : names)
                if (nid == id) {
                    name = n;
                    break;
                }
            out.push_back({name, base + name + "." + hash + ".js"});
        }
        return out;
    }
    return {};
}

QueryFailure classifyFailure(int status, std::string_view body, std::string* variable) {
    if (status == 412 || containsNoCase(body, "invalid query hash") || containsNoCase(body, "PersistedQueryNotFound") ||
        containsNoCase(body, "persisted query not found"))
        return QueryFailure::HashRejected;
    if (status == 400 || (status >= 200 && status < 300)) {
        // e.g. "Variable '$libraryItemUris' of required type '[String!]!' was not provided." or
        // VALIDATION_INVALID_TYPE_VARIABLE. The body names the variable: the request has to change, not the hash.
        std::string name;
        for (size_t d = body.find('$'); d != std::string_view::npos && name.empty(); d = body.find('$', d + 1)) {
            size_t e = d + 1;
            while (e < body.size() && isIdentChar(body[e])) ++e;
            if (e > d + 1 && !(body[d + 1] >= '0' && body[d + 1] <= '9')) name.assign(body.substr(d + 1, e - d - 1));
        }
        if (!name.empty() || containsNoCase(body, "variable") || body.find("VALIDATION_") != std::string_view::npos) {
            if (variable) *variable = name;
            return QueryFailure::VariableError;
        }
    }
    return QueryFailure::Other;
}

Discovery discoverHashes(const std::vector<std::string>& wanted, const CT& ct, int maxChunks) {
    Discovery d;
    auto missing = [&] {
        std::vector<std::string> m;
        for (const auto& w : wanted)
            if (!d.ops.contains(w)) m.push_back(w);
        return m;
    };
    auto isWanted = [&](const std::string& op) { return std::find(wanted.begin(), wanted.end(), op) != wanted.end(); };

    std::string webError;
    bool reachable = true;   // false after a transport failure or the chunk deadline: no registry fetch either
    try {
        const auto bundles = findBundleUrls(fetchText(kWebPlayer, ct));
        if (bundles.empty()) throw std::runtime_error("no web-player bundle referenced by open.spotify.com");
        std::string mainJs;
        for (const auto& url : bundles) {
            std::string js = fetchText(url, ct);
            for (auto& op : extractPersistedOps(js)) d.ops.emplace(op.name, std::move(op));
            if (d.bundle.empty()) {
                d.bundle = fileName(url);
                mainJs = std::move(js);
            }
        }
        d.complete = true;
        // Ops of lazily loaded routes live in chunk files. Scan the ones whose names match a missing op first
        // ("searchDesktop" -> "xpui-routes-search"), then named chunks, then numbered ones; stop once all are found.
        if (auto miss = missing(); !miss.empty() && maxChunks > 0) {
            std::vector<std::string> words;
            for (const auto& op : miss)
                for (auto& w : nameWords(op)) words.push_back(std::move(w));
            auto chunks = findChunkUrls(mainJs, bundles[0].substr(0, bundles[0].rfind('/') + 1));
            std::vector<std::pair<int, ChunkRef>> ranked;
            for (auto& c : chunks) ranked.emplace_back(chunkScore(c.name, words), std::move(c));
            std::stable_sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            chunks.clear();
            for (auto& [s, c] : ranked) chunks.push_back(std::move(c));
            if (chunks.empty()) ST_LOG_WARN("hashes", "no webpack chunk map found in {}", d.bundle);
            const auto deadline = std::chrono::steady_clock::now() + kChunkBudget;
            for (const auto& c : chunks) {
                if (d.chunksScanned >= maxChunks || missing().empty()) break;
                if (std::chrono::steady_clock::now() >= deadline) {
                    ST_LOG_WARN("hashes", "chunk scan stopped after {} chunk(s): {} s budget used", d.chunksScanned,
                                kChunkBudget.count());
                    d.complete = reachable = false;
                    break;
                }
                if (!allowedHost(c.url)) continue;
                ++d.chunksScanned;
                std::string js;
                try {
                    js = fetchText(c.url, ct);
                } catch (const StatusError& e) {
                    if (e.status >= 400 && e.status < 500) {   // this chunk only (gone / renamed)
                        ST_LOG_DEBUG("hashes", "chunk {} skipped: {}", c.name, e.what());
                        continue;
                    }
                    ST_LOG_WARN("hashes", "chunk scan stopped: {}", e.what());
                    d.complete = false;
                    break;
                } catch (const TransportError& e) {
                    ST_LOG_WARN("hashes", "chunk scan stopped: {}", e.what());
                    d.complete = reachable = false;
                    break;
                }
                for (auto& op : extractPersistedOps(js)) {
                    const std::string name = op.name;
                    if (d.ops.emplace(name, std::move(op)).second && isWanted(name)) d.fromChunks.push_back(name);
                }
            }
        }
    } catch (const TransportError& e) {
        webError = e.what();
        reachable = false;
        d.complete = false;
        ST_LOG_WARN("hashes", "web-player scan failed: {}", webError);
    } catch (const std::exception& e) {   // an error status, or the page / bundle no longer has the expected shape
        if (ct.isCancellationRequested()) throw;
        webError = e.what();
        d.complete = false;
        ST_LOG_WARN("hashes", "web-player scan failed: {}", webError);
    }

    // Last resort for the wanted ops still missing — only those: the registry is third-party data.
    if (const auto miss = missing(); reachable && !miss.empty()) {
        try {
            const std::string body = fetchText(kCommunityRegistry, ct);
            if (body.size() > kMaxRegistryBytes)
                throw std::runtime_error("response too large (" + std::to_string(body.size()) + " bytes)");
            const json j = json::parse(body, nullptr, false);
            const json& ops = j.is_object() && j.contains("operations") ? j["operations"] : json();
            for (const auto& name : miss) {
                if (!ops.is_object() || !ops.contains(name)) continue;
                const json& v = ops[name];
                const json& h = v.is_object() && v.contains("hash") ? v["hash"] : json();
                if (!h.is_string() || !isHex64(h.get<std::string>())) continue;
                const std::string kind =
                    v.contains("type") && v["type"].is_string() ? v["type"].get<std::string>() : "query";
                d.ops.emplace(name, PersistedOp{name, kind, h.get<std::string>()});
                d.fromRegistry.push_back(name);
            }
        } catch (const std::exception& e) {
            if (ct.isCancellationRequested()) throw;
            ST_LOG_WARN("hashes", "community registry unavailable: {}", e.what());
        }
    }
    if (d.ops.empty()) throw std::runtime_error("Spotify hash discovery failed: " + webError);
    return d;
}

// ---- Registry ----------------------------------------------------------------------------------------------------

HashRegistry& HashRegistry::instance() {
    static HashRegistry registry;
    return registry;
}

const std::vector<std::pair<const char*, const char*>>& HashRegistry::builtins() { return kBuiltins; }

std::string HashRegistry::builtin(std::string_view op) {
    for (const auto& [name, hash] : kBuiltins)
        if (op == name) return hash;
    return {};
}

std::string HashRegistry::lookup(std::string_view op) const {
    if (auto it = adopted_.find(op); it != adopted_.end()) return it->second;
    if (std::string b = builtin(op); !b.empty()) return b;
    if (auto it = discovered_.find(op); it != discovered_.end()) return it->second;
    return {};
}

std::string HashRegistry::hash(std::string_view op) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    return lookup(op);
}

std::string HashRegistry::bundle() {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    return bundle_;
}

void HashRegistry::ensureLoaded() {
    if (loaded_) return;
    loaded_ = true;
    if (path_.empty()) path_ = paths::appData() / L"spotify-hashes.json";
    std::ifstream f(path_, std::ios::binary);
    if (!f) return;
    const json j = json::parse(f, nullptr, false);
    if (!j.is_object()) {
        ST_LOG_WARN("hashes", "ignoring unreadable {}", path_.filename().string());
        return;
    }
    if (j.contains("bundle") && j["bundle"].is_string()) bundle_ = j["bundle"].get<std::string>();
    if (j.contains("fetchedAt") && j["fetchedAt"].is_number_integer()) fetchedAt_ = j["fetchedAt"].get<int64_t>();
    auto strings = [&](const char* key) {
        std::map<std::string, std::string, std::less<>> m;
        if (j.contains(key) && j[key].is_object())
            for (const auto& [op, h] : j[key].items())
                if (h.is_string()) m[op] = h.get<std::string>();
        return m;
    };
    for (auto& [op, h] : strings("discovered"))
        if (isHex64(h)) discovered_[op] = h;
    if (j.contains("searched") && j["searched"].is_array())
        for (const auto& op : j["searched"])
            if (op.is_string()) searched_.insert(op.get<std::string>());
    const auto replaced = strings("replaced");
    for (auto& [op, h] : strings("hashes")) {
        if (!isHex64(h)) continue;
        // A learned hash only overrides the built-in it replaced: a newer build with an updated table wins.
        const std::string b = builtin(op);
        const auto r = replaced.find(op);
        if (!b.empty() && (r == replaced.end() || r->second != b)) {
            ST_LOG_INFO("hashes", "{}: dropping learned {} (this build's built-in hash changed)", op, prefix(h));
            continue;
        }
        adopted_[op] = h;
        if (!b.empty()) replaced_[op] = b;
    }
    if (!adopted_.empty())
        ST_LOG_INFO("hashes", "{} learned hash(es) in use (bundle {})", adopted_.size(), bundle_.empty() ? "?" : bundle_);
}

void HashRegistry::save() {
    json hashes = json::object(), replaced = json::object(), discovered = json::object();
    for (const auto& [op, h] : adopted_)
        if (!transient_.contains(op)) hashes[op] = h;
    for (const auto& [op, h] : replaced_)
        if (hashes.contains(op)) replaced[op] = h;
    for (const auto& [op, h] : discovered_) discovered[op] = h;
    const json j{{"bundle", bundle_},           {"fetchedAt", fetchedAt_},
                 {"hashes", std::move(hashes)}, {"replaced", std::move(replaced)},
                 {"discovered", std::move(discovered)}, {"searched", searched_}};
    // Write a temp file and swap it in only when it was written completely: a full disk must not cost the old file.
    std::filesystem::path tmp = path_;
    tmp += L".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (out) out << j.dump(1);
        out.close();
        if (out.fail()) {
            ST_LOG_WARN("hashes", "cannot write {}; keeping the previous file", tmp.filename().string());
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    std::filesystem::rename(tmp, path_, ec);   // replaces the old file
    if (ec) {
        ST_LOG_WARN("hashes", "cannot save {}: {}", path_.filename().string(), ec.message());
        std::filesystem::remove(tmp, ec);
    }
}

void HashRegistry::adopt(const std::string& op, const std::string& hash, std::string_view source) {
    const std::string before = lookup(op);
    const std::string b = builtin(op);
    transient_.erase(op);
    if (hash == b) {
        adopted_.erase(op);
        replaced_.erase(op);
    } else {
        adopted_[op] = hash;
        if (b.empty()) replaced_.erase(op);
        else replaced_[op] = b;
    }
    ST_LOG_INFO("hashes", "{}: {} -> {} ({})", op, prefix(before), prefix(hash), source);
}

std::string HashRegistry::heal(const std::string& op, const std::string& rejectedHash, const CT& ct) {
    std::lock_guard healLock(healMutex_);
    std::string known;   // a different hash from the last scan (maybe an old one, from disk)
    {
        std::lock_guard lock(mutex_);
        ensureLoaded();
        // Another query healed this op while we waited for healMutex_.
        if (std::string current = lookup(op); !current.empty() && current != rejectedHash) return current;
        if (auto it = discovered_.find(op); it != discovered_.end() && it->second != rejectedHash) known = it->second;
        const auto now = std::chrono::steady_clock::now();
        const int64_t age = unixNow() - fetchedAt_;
        const bool scanFresh =
            fetchedAt_ > 0 && age >= 0 && age < std::chrono::duration_cast<std::chrono::seconds>(kRefreshInterval).count();
        const auto last = lastAttempt_.find(op);
        const bool opThrottled = last != lastAttempt_.end() && now - last->second < kRefreshInterval;
        if (!known.empty() && (scanFresh || opThrottled)) {   // a recent scan already has the answer
            adopt(op, known, "last scan of " + (bundle_.empty() ? std::string("?") : bundle_));
            save();
            return known;
        }
        if (opThrottled) {
            ST_LOG_WARN("hashes", "{}: hash {} rejected; no new scan (scanned for it {} s ago)", op, prefix(rejectedHash),
                        std::chrono::duration_cast<std::chrono::seconds>(now - last->second).count());
            return {};
        }
        // A recent scan only settles it when it actually looked for this op (a scan for another op may not have
        // opened the chunk this one lives in).
        if (scanFresh && searched_.contains(op)) {
            ST_LOG_WARN("hashes", "{}: hash {} rejected; no new scan ({} was searched for it {} s ago)", op,
                        prefix(rejectedHash), bundle_.empty() ? std::string("?") : bundle_, age);
            return {};
        }
        lastAttempt_[op] = now;
    }

    ST_LOG_INFO("hashes", "{}: hash {} rejected, scanning the web player for the current one", op, prefix(rejectedHash));
    const auto t0 = std::chrono::steady_clock::now();
    Discovery d;
    try {
        d = discoverHashes({op}, ct);
    } catch (const std::exception& e) {
        std::lock_guard lock(mutex_);
        if (ct.isCancellationRequested()) {
            lastAttempt_.erase(op);   // an aborted scan doesn't count against the throttle
            throw;
        }
        ST_LOG_WARN("hashes", "{}: refresh failed: {}", op, e.what());
        if (known.empty()) return {};
        adopt(op, known, "older scan of " + (bundle_.empty() ? std::string("?") : bundle_));   // better than nothing
        save();
        return known;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    auto fromRegistry = [&](const std::string& name) {
        return std::find(d.fromRegistry.begin(), d.fromRegistry.end(), name) != d.fromRegistry.end();
    };

    std::lock_guard lock(mutex_);
    // Only a web-player result describes a bundle (registry fill-ins are third-party data: adopted, never cached).
    if (!d.bundle.empty()) {
        if (d.bundle != bundle_) {   // a new deploy: forget the old scan
            discovered_.clear();
            searched_.clear();
        }
        for (const auto& [name, o] : d.ops)
            if (!fromRegistry(name)) {
                discovered_[name] = o.hash;
                searched_.insert(name);
            }
        if (d.complete) searched_.insert(op);
        bundle_ = d.bundle;
        fetchedAt_ = unixNow();
    }
    std::string extra;
    for (const auto& n : d.fromChunks) extra += (extra.empty() ? ", in chunks: " : " ") + n;
    for (const auto& n : d.fromRegistry) extra += " registry:" + n;
    ST_LOG_INFO("hashes", "scanned {}: {} ops, {} chunk(s) downloaded{}{} in {} ms",
                d.bundle.empty() ? "(web player unreachable)" : d.bundle, d.ops.size(), d.chunksScanned,
                d.complete ? "" : " (incomplete)", extra, ms);

    std::string fresh, source;
    if (const auto it = d.ops.find(op); it != d.ops.end()) {
        fresh = it->second.hash;
        source = fromRegistry(op) ? std::string("community registry") : d.bundle;
    } else if (!known.empty() && (d.bundle.empty() || !d.complete)) {   // the scan fell short: an older scan's answer
        fresh = known;
        source = "older scan";
    }
    if (fresh.empty()) {
        ST_LOG_WARN("hashes", "{}: not found in the web player nor the community registry", op);
    } else if (fresh == rejectedHash) {
        ST_LOG_WARN("hashes", "{}: the web player still sends {}: the rejection is not a rotated hash", op,
                    prefix(rejectedHash));
        fresh.clear();
    } else {
        adopt(op, fresh, source);
    }
    save();
    return fresh;
}

void HashRegistry::setStorePath(std::filesystem::path path) {
    std::lock_guard healLock(healMutex_);
    std::lock_guard lock(mutex_);
    path_ = std::move(path);
    loaded_ = false;
    adopted_.clear();
    replaced_.clear();
    discovered_.clear();
    searched_.clear();
    transient_.clear();
    bundle_.clear();
    fetchedAt_ = 0;
    lastAttempt_.clear();
}

void HashRegistry::setOverride(const std::string& op, const std::string& hash) {
    std::lock_guard lock(mutex_);
    ensureLoaded();
    adopted_[op] = hash;
    transient_.insert(op);
}

} // namespace st::spotify
