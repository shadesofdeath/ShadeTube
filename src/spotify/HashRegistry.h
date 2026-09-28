#pragma once
// Pathfinder persisted-query hashes, with self-healing.
//
// Every Pathfinder GraphQL call names an operation and sends the sha256 of its query document ("persisted query").
// A web-player deploy that changes a document changes its hash; Spotify keeps serving retired hashes for a while,
// then purges them, and the op fails with HTTP 412 "Invalid query hash" (verified 2026-09-28). Instead of needing a
// rebuild, the registry:
//   * starts from the built-in table below (the hashes this build was verified with);
//   * learns the current ones from the live web player: GET open.spotify.com -> web-player.<hash>.js -> every
//     `new X.l("<op>","query"|"mutation","<sha256>",null)`. Ops in lazily loaded webpack chunks (searchDesktop lives
//     in xpui-routes-search.<hash>.js) are found through the bundle's chunk map (bounded, only when needed). Last
//     resort: the community registry github.com/Jigen-Ohtsusuki/spotify-gql-registry (hashes.json);
//   * adopts a learned hash for an op only once that op's current hash was rejected — a working op never switches
//     to a newer document whose required variables may differ — and persists it in
//     paths::appData()/spotify-hashes.json {bundle, fetchedAt, hashes:{op:hash}, replaced, discovered, searched}.
// Thread-safe: queries run concurrently on pool threads. Only heal() touches the network; Api::query calls it from
// worker threads (never from the UI thread).
#include <YoutubeExplode/Common/Cancellation.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::spotify {

struct PersistedOp {
    std::string name;
    std::string kind;   // "query" | "mutation"
    std::string hash;   // 64 lowercase hex
};

// ---- Discovery (the parsers are pure and exposed for tests) ----------------------------------------------------

// Every persisted operation in a JS source (`"<name>","query"|"mutation","<64 hex>"`), in source order. A name that
// appears again (the bundle repeats some modules verbatim) keeps its first hash.
std::vector<PersistedOp> extractPersistedOps(std::string_view js);

// Absolute URLs of the web-player bundles open.spotify.com's HTML loads (…/cdn/build/web-player/web-player.<h>.js;
// the vendor~ / encore~ library bundles are skipped: they define no operations). References on any host other than
// open.spotifycdn.com / open.spotify.com are ignored; relative ones resolve to open.spotifycdn.com.
std::vector<std::string> findBundleUrls(std::string_view html);

// The lazily loaded chunks of the webpack runtime in `bundleJs`:
//   u.u=e=>""+(({4406:"xpui-routes-search",...})[e]||e)+"."+({4406:"96d81afe",...})[e]+".js"
// resolved against the runtime's public path (u.p="https://…/") or, when it has none or it is not a Spotify host
// (open.spotifycdn.com / open.spotify.com), `baseUrl`. Source order.
struct ChunkRef {
    std::string name;   // "xpui-routes-search", or the numeric chunk id for unnamed chunks
    std::string url;
};
std::vector<ChunkRef> findChunkUrls(std::string_view bundleJs, std::string_view baseUrl);

// How Pathfinder rejected a request.
enum class QueryFailure {
    Other,
    HashRejected,    // unknown / purged hash: HTTP 412 "Invalid query hash" (or a PersistedQueryNotFound error)
    VariableError,   // HTTP 400 naming a missing / mistyped variable: the request must change, not the hash
};
// `variable` (optional) receives the variable a VariableError names, when the body names one.
QueryFailure classifyFailure(int status, std::string_view body, std::string* variable = nullptr);

struct Discovery {
    std::string bundle;                       // "web-player.da8b87c8.js" ("" when the web player was unreachable)
    std::map<std::string, PersistedOp> ops;   // by name: everything the web player defines + registry fill-ins
    int chunksScanned = 0;
    bool complete = false;                    // the web scan looked everywhere it may (no transport error/deadline)
    std::vector<std::string> fromChunks;      // wanted ops found in chunks (not in the main bundle)
    std::vector<std::string> fromRegistry;    // wanted ops filled in from the GitHub registry (not web-player data)
};
// Blocking network scan: open.spotify.com + the main bundle, then — only while some op in `wanted` is missing —
// up to `maxChunks` chunk files (the ones whose names match the missing ops first; ~45 s budget; a transport error
// ends the loop, a chunk's HTTP 4xx only skips it), then the GitHub registry for the `wanted` ops still missing
// (skipped after a transport error or the deadline). Only https://open.spotifycdn.com / open.spotify.com URLs are
// fetched from the page. Throws only when nothing at all could be discovered.
Discovery discoverHashes(const std::vector<std::string>& wanted, const YoutubeExplode::CancellationToken& ct,
                         int maxChunks = 40);

// ---- Registry ---------------------------------------------------------------------------------------------------

class HashRegistry {
public:
    static HashRegistry& instance();

    // The hash to send for `op`: an adopted (learned) one, else the built-in one, else one the last scan
    // discovered. "" when the op is unknown.
    std::string hash(std::string_view op);
    static std::string builtin(std::string_view op);
    static const std::vector<std::pair<const char*, const char*>>& builtins();   // {op, hash}, the built-in table

    // `op` was rejected with `rejectedHash` (QueryFailure::HashRejected). Returns the hash to retry with, or "" when
    // there is no different one. Scans the web player when needed: at most once per 10 minutes per op in this
    // process, and not when a scan of the current bundle less than 10 minutes old (also from spotify-hashes.json)
    // already looked for this op. A recent scan that knows a different hash answers directly; an older scan's hash
    // is only the fallback when the new scan fails. Concurrent callers wait for one scan and share its result.
    // Blocking: worker threads only.
    std::string heal(const std::string& op, const std::string& rejectedHash, const YoutubeExplode::CancellationToken& ct);

    // ---- tests / diagnostics ----
    // Where learned hashes persist (default: paths::appData()/spotify-hashes.json; empty = back to the default).
    // Drops the in-memory state; the file is (re)read on the next use.
    void setStorePath(std::filesystem::path path);
    // Makes `op` use `hash` (in memory only, like an adopted hash that was never saved).
    void setOverride(const std::string& op, const std::string& hash);
    std::string bundle();   // the bundle of the last scan ("" = none yet)

    static constexpr std::chrono::minutes kRefreshInterval{10};

private:
    HashRegistry() = default;
    // All four: mutex_ held.
    void ensureLoaded();
    void save();
    std::string lookup(std::string_view op) const;
    void adopt(const std::string& op, const std::string& hash, std::string_view source);

    std::mutex mutex_;       // the maps below (held briefly; never across network I/O)
    std::mutex healMutex_;   // serializes heal(): one refresh at a time, waiters reuse its result
    bool loaded_ = false;
    std::filesystem::path path_;
    std::map<std::string, std::string, std::less<>> adopted_;      // op -> learned hash (wins over the built-in)
    std::map<std::string, std::string, std::less<>> replaced_;     // op -> the built-in it replaced
    // The last web-player scan of `bundle_` (never community-registry data): what it found, and which ops it looked
    // for (every wanted op of a complete scan + everything it found). Both cleared when the bundle changes.
    std::map<std::string, std::string, std::less<>> discovered_;
    std::set<std::string, std::less<>> searched_;
    std::set<std::string, std::less<>> transient_;   // setOverride() ops: never saved
    std::string bundle_;
    int64_t fetchedAt_ = 0;   // unix seconds of the last web-player scan (from disk or this process)
    std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> lastAttempt_;   // per op, this process
};

} // namespace st::spotify
