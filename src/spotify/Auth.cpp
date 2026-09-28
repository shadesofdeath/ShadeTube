#include "spotify/Auth.h"

#include "core/Http.h"
#include "core/Log.h"
#include "spotify/Totp.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace st::spotify {
namespace {

using json = nlohmann::json;

// Baked-in fallback secret, used if the gist can't be reached. Kept current from the maintainers' list.
constexpr int kFallbackNuanceVersion = 61;
constexpr const char* kFallbackNuanceSecret =
    "GM3TMMJTGYZTQNZVGM4DINJZHA4TGOBYGMZTCMRTGEYDSMJRHE4TEOBUG4YTCMRUGQ4DQOJUGQYTAMRRGA2TCMJSHE3TCMBY";

// The maintainers publish the rotating secret list here; we pick the entry with the highest version.
constexpr const char* kNuanceGist =
    "https://gist.githubusercontent.com/raw/22ed9c6ba463899e933427f7de1f0eef/nuances.json";

constexpr const char* kServerTimeUrl = "https://open.spotify.com/api/server-time";
constexpr const char* kTokenUrl = "https://open.spotify.com/api/token";

std::mutex g_nuanceMutex;
std::optional<Nuance> g_cachedNuance;

http::Headers browserHeaders() {
    return {
        {"User-Agent", kUserAgent},
        {"Accept", "application/json"},
        {"Referer", "https://open.spotify.com/"},
        {"App-Platform", "WebPlayer"},
    };
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

Nuance fetchNuance(bool bustCache, const CT& ct) {
    if (!bustCache) {
        std::lock_guard lock(g_nuanceMutex);
        if (g_cachedNuance) return *g_cachedNuance;
    }

    Nuance best{kFallbackNuanceVersion, kFallbackNuanceSecret};
    try {
        std::string url = kNuanceGist;
        if (bustCache) url += "?t=" + std::to_string(nowMs());
        const auto resp = http::get(url, {{"User-Agent", kUserAgent}, {"Accept", "application/json"}}, ct);
        if (resp.isSuccessStatusCode()) {
            const auto j = json::parse(resp.body, nullptr, false);
            if (j.is_array()) {
                int bestV = -1;
                for (const auto& e : j) {
                    const int v = e.value("v", -1);
                    const std::string s = e.value("s", std::string{});
                    if (v > bestV && !s.empty()) {
                        bestV = v;
                        best = Nuance{v, s};
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "nuance fetch failed ({}); using baked-in v{}", e.what(), kFallbackNuanceVersion);
    }

    std::lock_guard lock(g_nuanceMutex);
    g_cachedNuance = best;
    return best;
}

int64_t fetchServerTimeSeconds(const CT& ct) {
    try {
        const auto resp = http::get(kServerTimeUrl, browserHeaders(), ct);
        if (resp.isSuccessStatusCode()) {
            const auto j = json::parse(resp.body, nullptr, false);
            if (j.is_object() && j.contains("serverTime")) return j["serverTime"].get<int64_t>();
        }
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "server-time fetch failed ({}); using local clock", e.what());
    }
    return nowMs() / 1000;
}

namespace {

TokenBundle requestToken(const std::string& spDc, const Nuance& nuance, const CT& ct) {
    const int64_t serverTime = fetchServerTimeSeconds(ct);
    const std::string code = totp(nuance.secret, serverTime);

    std::string url = kTokenUrl;
    url += "?reason=transport&productType=web-player";
    url += "&totp=" + code;
    url += "&totpServer=" + code;
    url += "&totpVer=" + std::to_string(nuance.version);

    auto headers = browserHeaders();
    headers.emplace_back("Cookie", "sp_dc=" + spDc + ";");

    const auto resp = http::get(url, headers, ct);
    if (!resp.isSuccessStatusCode())
        throw std::runtime_error("token endpoint HTTP " + std::to_string(resp.statusCode));

    const auto j = json::parse(resp.body, nullptr, false);
    if (!j.is_object() || !j.contains("accessToken"))
        throw std::runtime_error("token response missing accessToken");

    TokenBundle t;
    t.accessToken = j.value("accessToken", std::string{});
    t.expiresAtMs = j.value("accessTokenExpirationTimestampMs", int64_t{0});
    t.anonymous = j.value("isAnonymous", false);
    if (t.accessToken.empty()) throw std::runtime_error("empty accessToken");
    return t;
}

} // namespace

TokenBundle fetchAccessToken(const std::string& spDc, const CT& ct) {
    if (spDc.empty()) throw std::runtime_error("sp_dc cookie is empty");

    try {
        TokenBundle t = requestToken(spDc, fetchNuance(false, ct), ct);
        if (t.anonymous) throw std::runtime_error("anonymous token (sp_dc rejected)");
        return t;
    } catch (const std::exception& e) {
        ST_LOG_WARN("spotify", "token attempt 1 failed ({}); retrying with fresh nuance", e.what());
        TokenBundle t = requestToken(spDc, fetchNuance(true, ct), ct);
        if (t.anonymous) throw std::runtime_error("anonymous token (sp_dc rejected or expired) — re-login needed");
        return t;
    }
}

} // namespace st::spotify
