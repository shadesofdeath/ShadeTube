#include "app/Scrobbler.h"

#include "core/Dispatcher.h"
#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <windows.h>
// <windows.h> must precede <bcrypt.h> / <wincrypt.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace st::app {

using json = nlohmann::json;
using scrobble::Params;

namespace {

constexpr const char* kTag = "scrobble";
constexpr const char* kLastfmApi = "https://ws.audioscrobbler.com/2.0/";
constexpr const char* kLastfmAuthPage = "https://www.last.fm/api/auth/";
constexpr const char* kLbApi = "https://api.listenbrainz.org/1/";
constexpr const char* kUserAgent = "ShadeTube/0.2 (Windows; scrobbler)";
constexpr size_t kMaxQueue = 500;       // per service; oldest dropped beyond this
constexpr size_t kLastfmBatch = 50;     // track.scrobble accepts up to 50 per call
constexpr size_t kLbBatch = 100;

int64_t unixNow() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string trim(std::string s) {
    const auto ws = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && ws(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
}

std::string dumpJson(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string jstr(const json& o, const char* key) {
    if (!o.is_object()) return {};
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

void wipe(std::string& s) {
    if (!s.empty()) SecureZeroMemory(s.data(), s.size());
    s.clear();
}

bool validTrack(const ScrobbleTrack& t) { return !t.title.empty() && !t.artist.empty(); }

void postGuarded(Lifetime::Ref ref, std::function<void()> fn) {
    Dispatcher::post([ref = std::move(ref), fn = std::move(fn)] {
        if (!ref.expired() && fn) fn();
    });
}

// --- DPAPI (CurrentUser) -----------------------------------------------------------------------------------

std::string protect(const std::string& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"ShadeTube scrobble", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN(kTag, "CryptProtectData failed: {}", GetLastError());
        return {};
    }
    std::string enc(reinterpret_cast<const char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return enc;
}

std::string unprotect(std::string enc) {
    DATA_BLOB in{static_cast<DWORD>(enc.size()), reinterpret_cast<BYTE*>(enc.data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        ST_LOG_WARN(kTag, "CryptUnprotectData failed: {}", GetLastError());
        return {};
    }
    std::string plain(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

// --- Last.fm -----------------------------------------------------------------------------------------------

struct LfmReply {
    int http = 0;
    int error = 0;          // Last.fm error code; -1 = unparseable / HTTP failure without a Last.fm error
    std::string message;    // Last.fm's own message (never contains credentials)
    json body;
};

// Worker thread. Signs, POSTs (form-encoded, format=json) and parses. Throws only on transport failure.
LfmReply lastfmCall(Params params, const std::string& secret) {
    params.emplace_back("api_sig", scrobble::lastfmSignature(params, secret));
    params.emplace_back("format", "json");
    const auto r = http::postForm(kLastfmApi, params, {{"User-Agent", kUserAgent}});
    LfmReply out;
    out.http = r.statusCode;
    json j = json::parse(r.body, nullptr, false);
    if (j.is_object()) {
        out.body = std::move(j);
        if (const auto e = out.body.find("error"); e != out.body.end()) {
            out.error = e->is_number_integer() ? e->get<int>() : -1;
            out.message = jstr(out.body, "message");
        }
    }
    if (out.error == 0 && !r.isSuccessStatusCode()) {
        out.error = -1;
        out.message = "HTTP " + std::to_string(r.statusCode);
    } else if (out.error == 0 && !out.body.is_object()) {
        // Every format=json reply is an object: an empty/HTML 200 (outage page) is not a success - keep the
        // scrobbles queued instead of dropping them.
        out.error = -1;
        out.message = toUtf8(i18n::format(tr(L"okunamayan yanıt (HTTP {})"), r.statusCode));   // shown via lastfmErrorText
    }
    return out;
}

// User-facing text (UI language) for a failed Last.fm call.
std::string lastfmErrorText(const LfmReply& r) {
    switch (r.error) {
    case 10:
    case 26: return toUtf8(tr(L"Last.fm API anahtarı geçersiz. last.fm/api/account/create sayfasındaki anahtarı kontrol et."));
    case 13: return toUtf8(tr(L"Last.fm imzası reddedildi: gizli anahtarı (shared secret) kontrol et."));
    case 14: return toUtf8(tr(L"Last.fm'de erişime henüz izin vermedin. Tarayıcıda \"Yes, allow access\" deyip tekrar dene."));
    case 4:
    case 15: return toUtf8(tr(L"Onay isteğinin süresi doldu. \"Bağlan\" ile yeniden başla."));
    case 11:
    case 16: return toUtf8(tr(L"Last.fm şu anda yanıt vermiyor. Biraz sonra tekrar dene."));
    case 29: return toUtf8(tr(L"Last.fm istek sınırına ulaşıldı. Biraz sonra tekrar dene."));
    default:
        // Last.fm's own message (English) or the HTTP status as the detail.
        return toUtf8(i18n::format(tr(L"Last.fm hatası: {}"),
                                   {r.message.empty() ? L"HTTP " + std::to_wstring(r.http) : toWide(r.message)}));
    }
}

void addTrackParams(Params& p, const ScrobbleTrack& t, const std::string& suffix) {
    p.emplace_back("artist" + suffix, t.artist);
    p.emplace_back("track" + suffix, t.title);
    if (!t.album.empty()) p.emplace_back("album" + suffix, t.album);
    if (t.durationMs >= 1000) p.emplace_back("duration" + suffix, std::to_string(t.durationMs / 1000));
}

// --- ListenBrainz ------------------------------------------------------------------------------------------

struct LbReply {
    int http = 0;
    std::string message;    // server message (never contains the token)
};

struct LbValidation {
    int http = 0;
    bool valid = false;
    std::string user;
    std::string message;
};

http::Headers lbHeaders(const std::string& token) {
    return {{"Authorization", "Token " + token}, {"User-Agent", kUserAgent}};
}

LbValidation lbValidate(const std::string& token) {
    const auto r = http::get(std::string(kLbApi) + "validate-token", lbHeaders(token));
    LbValidation v;
    v.http = r.statusCode;
    const json j = json::parse(r.body, nullptr, false);
    if (j.is_object()) {
        if (const auto it = j.find("valid"); it != j.end() && it->is_boolean()) v.valid = it->get<bool>();
        v.user = jstr(j, "user_name");
        v.message = jstr(j, "message");
        if (v.message.empty()) v.message = jstr(j, "error");
    }
    if (!r.isSuccessStatusCode()) v.valid = false;
    return v;
}

LbReply lbSubmit(const std::string& token, std::string body) {
    http::HttpRequest req;
    req.method = "POST";
    req.url = std::string(kLbApi) + "submit-listens";
    req.headers = lbHeaders(token);
    req.headers.emplace_back("Content-Type", "application/json");
    req.body = std::move(body);
    const auto r = http::send(std::move(req));
    LbReply out;
    out.http = r.statusCode;
    const json j = json::parse(r.body, nullptr, false);
    if (j.is_object()) {
        out.message = jstr(j, "error");
        if (out.message.empty()) out.message = jstr(j, "message");
    }
    return out;
}

json lbListen(const ScrobbleTrack& t, int64_t listenedAt /* 0 = playing_now */) {
    json info = {{"submission_client", "ShadeTube"}, {"submission_client_version", "0.2"}, {"media_player", "ShadeTube"}};
    if (t.durationMs > 0) info["duration_ms"] = t.durationMs;
    json meta = {{"artist_name", t.artistCredit.empty() ? t.artist : t.artistCredit},
                 {"track_name", t.title},
                 {"additional_info", std::move(info)}};
    if (!t.album.empty()) meta["release_name"] = t.album;
    json item = {{"track_metadata", std::move(meta)}};
    if (listenedAt > 0) item["listened_at"] = listenedAt;
    return item;
}

} // namespace

// ============================================================================================================
// Free helpers

ScrobbleTrack scrobbleTrackFrom(const catalog::Track& t) {
    ScrobbleTrack s;
    s.id = t.id;
    s.title = t.name;
    if (!t.artists.empty()) s.artist = t.artists.front().name;
    s.artistCredit = t.artistLine();
    if (s.artist.empty()) s.artist = s.artistCredit;
    s.album = t.album.name;
    s.durationMs = t.durationMs;
    return s;
}

namespace scrobble {

std::string md5Hex(std::string_view bytes) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS st = BCryptOpenAlgorithmProvider(&alg, BCRYPT_MD5_ALGORITHM, nullptr, 0);
    if (st < 0) throw std::runtime_error("BCryptOpenAlgorithmProvider(MD5) failed");
    UCHAR digest[16] = {};
    BCRYPT_HASH_HANDLE hash = nullptr;
    st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (st >= 0) {
        st = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())), static_cast<ULONG>(bytes.size()), 0);
        if (st >= 0) st = BCryptFinishHash(hash, digest, sizeof digest, 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) throw std::runtime_error("MD5 computation failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (UCHAR b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

std::string lastfmSignature(const Params& params, std::string_view secret) {
    std::vector<const std::pair<std::string, std::string>*> sorted;
    sorted.reserve(params.size());
    for (const auto& p : params)
        if (p.first != "format" && p.first != "callback") sorted.push_back(&p);
    std::stable_sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->first < b->first; });
    std::string s;
    for (const auto* p : sorted) {
        s += p->first;
        s += p->second;
    }
    s += secret;
    std::string sig = md5Hex(s);
    wipe(s);   // contains the secret
    return sig;
}

void ListenClock::reset(int64_t durationMs, int64_t wallMs) {
    *this = ListenClock{};
    durationMs_ = std::max<int64_t>(0, durationMs);
    lastWallMs_ = wallMs;
}

void ListenClock::learnDuration(int64_t durationMs) {
    if (durationMs_ <= 0 && durationMs > 0) durationMs_ = durationMs;
}

int64_t ListenClock::thresholdMs() const {
    if (durationMs_ <= 0) return kMaxThresholdMs;   // unknown length: the 4-minute rule alone
    if (durationMs_ <= kMinTrackMs) return -1;
    return std::min(durationMs_ / 2, kMaxThresholdMs);
}

bool ListenClock::update(int64_t positionMs, bool playing, int64_t wallMs) {
    const int64_t delta = positionMs - lastPosMs_;
    const int64_t elapsed = std::max<int64_t>(0, wallMs - lastWallMs_);
    // Count only real playback: the position advanced while playing, by no more than the wall clock did.
    if ((playing || lastPlaying_) && delta > 0 && delta <= elapsed + kSeekToleranceMs) listenedMs_ += delta;
    lastPosMs_ = positionMs;
    lastWallMs_ = wallMs;
    lastPlaying_ = playing;
    const int64_t threshold = thresholdMs();
    if (!fired_ && threshold >= 0 && listenedMs_ >= threshold) {
        fired_ = true;
        return true;
    }
    return false;
}

} // namespace scrobble

// ============================================================================================================
// Scrobbler

Scrobbler::Scrobbler(std::filesystem::path storeFile) : file_(std::move(storeFile)) {
    if (file_.empty()) file_ = paths::appData() / L"scrobble.dat";
}

Scrobbler::~Scrobbler() {
    wipe(lfmSecret_);
    wipe(lfmSession_);
    wipe(lbToken_);
}

int64_t Scrobbler::nowMonotonic() const {
    if (clock) return clock();
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void Scrobbler::changed() {
    if (onChanged) onChanged();
}

void Scrobbler::load() {
    try {
        std::ifstream f(file_, std::ios::binary);
        if (!f) return;
        std::string enc((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (enc.empty()) return;
        std::string plain = unprotect(std::move(enc));
        if (plain.empty()) return;
        const json j = json::parse(plain, nullptr, false);
        wipe(plain);
        if (!j.is_object()) {
            ST_LOG_WARN(kTag, "scrobble.dat is unreadable; ignoring it");
            return;
        }
        if (const auto l = j.find("lastfm"); l != j.end() && l->is_object()) {
            lfmKey_ = jstr(*l, "apiKey");
            lfmSecret_ = jstr(*l, "secret");
            lfmSession_ = jstr(*l, "sessionKey");
            lfmUser_ = jstr(*l, "user");
        }
        if (const auto b = j.find("listenbrainz"); b != j.end() && b->is_object()) {
            lbToken_ = jstr(*b, "token");
            lbUser_ = jstr(*b, "user");
        }
        ++lfmGen_;
        ++lbGen_;
        ST_LOG_INFO(kTag, "credentials loaded (last.fm: {}, listenbrainz: {})",
                    lastfmConnected() ? "connected" : lastfmHasKeys() ? "keys only" : "off",
                    listenbrainzConnected() ? "connected" : "off");
    } catch (const std::exception& e) {
        ST_LOG_WARN(kTag, "load failed: {}", e.what());
    }
}

void Scrobbler::save() const {
    try {
        std::error_code ec;
        if (lfmKey_.empty() && lfmSecret_.empty() && lfmSession_.empty() && lbToken_.empty()) {
            std::filesystem::remove(file_, ec);
            return;
        }
        const json j = {
            {"v", 1},
            {"lastfm", {{"apiKey", lfmKey_}, {"secret", lfmSecret_}, {"sessionKey", lfmSession_}, {"user", lfmUser_}}},
            {"listenbrainz", {{"token", lbToken_}, {"user", lbUser_}}},
        };
        std::string plain = dumpJson(j);
        const std::string enc = protect(plain);
        wipe(plain);
        if (enc.empty()) return;
        if (file_.has_parent_path()) std::filesystem::create_directories(file_.parent_path(), ec);
        auto tmp = file_;
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(enc.data(), static_cast<std::streamsize>(enc.size()));
            if (!f) {
                ST_LOG_WARN(kTag, "could not write scrobble.dat");
                return;
            }
        }
        std::filesystem::rename(tmp, file_, ec);
        if (ec) ST_LOG_WARN(kTag, "could not replace scrobble.dat: {}", ec.message());
    } catch (const std::exception& e) {
        ST_LOG_WARN(kTag, "save failed: {}", e.what());
    }
}

// --- Last.fm -------------------------------------------------------------------------------------------------

bool Scrobbler::lastfmHasKeys() const { return !lfmKey_.empty() && !lfmSecret_.empty(); }
bool Scrobbler::lastfmConnected() const { return lastfmHasKeys() && !lfmSession_.empty(); }
std::string Scrobbler::lastfmUser() const { return lfmUser_; }
std::string Scrobbler::lastfmApiKey() const { return lfmKey_; }
bool Scrobbler::lastfmHasSecret() const { return !lfmSecret_.empty(); }
bool Scrobbler::lastfmAuthPending() const { return !lfmPendingToken_.empty(); }

void Scrobbler::lastfmSetKeys(std::string apiKey, std::string secret) {
    apiKey = trim(std::move(apiKey));
    secret = trim(std::move(secret));
    if (secret.empty() && apiKey == lfmKey_) secret = lfmSecret_;   // blank field = keep the stored secret
    if (apiKey == lfmKey_ && secret == lfmSecret_) return;
    lfmKey_ = std::move(apiKey);
    wipe(lfmSecret_);
    lfmSecret_ = std::move(secret);
    // A session key belongs to the API account that issued it.
    const bool hadSession = !lfmSession_.empty();
    wipe(lfmSession_);
    lfmUser_.clear();
    lfmPendingToken_.clear();
    lfmQueue_.clear();
    ++lfmGen_;
    save();
    ST_LOG_INFO(kTag, "last.fm keys updated{}", hadSession ? " (previous session dropped)" : "");
    changed();
}

void Scrobbler::lastfmBeginAuth(std::function<void(std::string url, std::string error)> done) {
    if (!lastfmHasKeys()) {
        postGuarded(life_.ref(), [done] {
            if (done) done({}, toUtf8(tr(L"Önce Last.fm API anahtarını ve gizli anahtarını gir.")));
        });
        return;
    }
    Params p{{"method", "auth.getToken"}, {"api_key", lfmKey_}};
    const std::string secret = lfmSecret_;
    const std::string key = lfmKey_;
    async(
        Priority::High, life_.ref(), [p = std::move(p), secret] { return lastfmCall(p, secret); },
        [this, key, done](Result<LfmReply> r) {
            auto finish = [&](std::string url, std::string err) {
                if (done) done(std::move(url), std::move(err));
            };
            if (!r) {
                ST_LOG_WARN(kTag, "last.fm auth.getToken failed: {}", r.errorMessage());
                return finish({}, toUtf8(tr(L"Last.fm'e ulaşılamadı. İnternet bağlantını kontrol et.")));
            }
            if (r->error) {
                ST_LOG_WARN(kTag, "last.fm auth.getToken error {}: {}", r->error, r->message);
                return finish({}, lastfmErrorText(*r));
            }
            const std::string token = jstr(r->body, "token");
            if (token.empty()) return finish({}, toUtf8(tr(L"Last.fm beklenmeyen bir yanıt verdi. Tekrar dene.")));
            if (key != lfmKey_) return finish({}, toUtf8(tr(L"API anahtarı bu sırada değişti. Tekrar dene.")));
            lfmPendingToken_ = token;
            ST_LOG_INFO(kTag, "last.fm auth token issued; waiting for browser approval");
            const std::string url =
                std::string(kLastfmAuthPage) + "?api_key=" + http::urlEncode(key) + "&token=" + http::urlEncode(token);
            changed();
            finish(url, {});
        });
}

void Scrobbler::lastfmFinishAuth(std::function<void(bool ok, std::string error)> done) {
    if (!lastfmHasKeys() || lfmPendingToken_.empty()) {
        postGuarded(life_.ref(), [done] {
            if (done) done(false, toUtf8(tr(L"Önce \"Bağlan\" ile Last.fm onay sayfasını aç.")));
        });
        return;
    }
    Params p{{"method", "auth.getSession"}, {"api_key", lfmKey_}, {"token", lfmPendingToken_}};
    const std::string secret = lfmSecret_;
    const std::string key = lfmKey_;
    const std::string token = lfmPendingToken_;
    async(
        Priority::High, life_.ref(), [p = std::move(p), secret] { return lastfmCall(p, secret); },
        [this, key, token, done](Result<LfmReply> r) {
            auto finish = [&](bool ok, std::string err) {
                if (done) done(ok, std::move(err));
            };
            if (!r) {
                ST_LOG_WARN(kTag, "last.fm auth.getSession failed: {}", r.errorMessage());
                return finish(false, toUtf8(tr(L"Last.fm'e ulaşılamadı. İnternet bağlantını kontrol et.")));
            }
            if (r->error) {
                ST_LOG_WARN(kTag, "last.fm auth.getSession error {}: {}", r->error, r->message);
                if ((r->error == 4 || r->error == 15) && lfmPendingToken_ == token) {
                    lfmPendingToken_.clear();   // dead token: the user has to start over
                    changed();
                }
                return finish(false, lastfmErrorText(*r));
            }
            const auto s = r->body.find("session");
            const std::string sk = s != r->body.end() ? jstr(*s, "key") : std::string{};
            const std::string name = s != r->body.end() ? jstr(*s, "name") : std::string{};
            if (sk.empty()) return finish(false, toUtf8(tr(L"Last.fm beklenmeyen bir yanıt verdi. Tekrar dene.")));
            if (key != lfmKey_) return finish(false, toUtf8(tr(L"API anahtarı bu sırada değişti. Tekrar bağlan.")));
            wipe(lfmSession_);
            lfmSession_ = sk;
            lfmUser_ = name;
            lfmPendingToken_.clear();
            ++lfmGen_;
            save();
            ST_LOG_INFO(kTag, "last.fm connected as {}", name);
            changed();
            finish(true, {});
            flushLastfm();
        });
}

void Scrobbler::lastfmDisconnect() {
    wipe(lfmSession_);
    lfmUser_.clear();
    lfmPendingToken_.clear();
    lfmQueue_.clear();
    ++lfmGen_;
    save();
    ST_LOG_INFO(kTag, "last.fm disconnected");
    changed();
}

void Scrobbler::dropLastfmSession(const char* why) {
    ST_LOG_WARN(kTag, "last.fm: {}; session dropped, reconnect in Settings", why);
    wipe(lfmSession_);
    lfmUser_.clear();
    ++lfmGen_;
    save();
    changed();
}

// --- ListenBrainz ------------------------------------------------------------------------------------------

bool Scrobbler::listenbrainzConnected() const { return !lbToken_.empty(); }
std::string Scrobbler::listenbrainzUser() const { return lbUser_; }

void Scrobbler::listenbrainzSetToken(std::string token, std::function<void(bool ok, std::string userOrError)> done) {
    token = trim(std::move(token));
    if (token.empty()) {
        postGuarded(life_.ref(), [done] {
            if (done) done(false, toUtf8(tr(L"ListenBrainz token'ını yapıştır (listenbrainz.org/settings).")));
        });
        return;
    }
    async(
        Priority::High, life_.ref(), [token] { return lbValidate(token); },
        [this, token, done](Result<LbValidation> r) {
            auto finish = [&](bool ok, std::string msg) {
                if (done) done(ok, std::move(msg));
            };
            if (!r) {
                ST_LOG_WARN(kTag, "listenbrainz validate-token failed: {}", r.errorMessage());
                return finish(false, toUtf8(tr(L"ListenBrainz'e ulaşılamadı. İnternet bağlantını kontrol et.")));
            }
            if (!r->valid) {
                ST_LOG_INFO(kTag, "listenbrainz token rejected (HTTP {}): {}", r->http, r->message);
                if (r->http >= 500 || r->http == 429)
                    return finish(false, toUtf8(i18n::format(tr(L"ListenBrainz şu anda yanıt vermiyor (HTTP {}). Biraz sonra tekrar dene."),
                                                             r->http)));
                return finish(false, toUtf8(tr(L"Token geçersiz. listenbrainz.org/settings sayfasındaki kullanıcı token'ını kopyala.")));
            }
            wipe(lbToken_);
            lbToken_ = token;
            lbUser_ = r->user;
            ++lbGen_;
            save();
            ST_LOG_INFO(kTag, "listenbrainz connected as {}", r->user);
            changed();
            finish(true, r->user);
            flushListenbrainz();
        });
}

void Scrobbler::listenbrainzDisconnect() {
    wipe(lbToken_);
    lbUser_.clear();
    lbQueue_.clear();
    ++lbGen_;
    save();
    ST_LOG_INFO(kTag, "listenbrainz disconnected");
    changed();
}

void Scrobbler::dropListenbrainzToken(const char* why) {
    ST_LOG_WARN(kTag, "listenbrainz: {}; token dropped, reconnect in Settings", why);
    wipe(lbToken_);
    lbUser_.clear();
    ++lbGen_;
    save();
    changed();
}

// --- Playback ------------------------------------------------------------------------------------------------

void Scrobbler::onTrackStarted(const ScrobbleTrack& t) {
    if (active_ && !t.id.empty() && t.id == track_.id) {
        // Same track again. A re-resolve / "Yanlış eşleşme?" switch / resume after an error continues at the
        // same position (same play: keep the listened time and do not scrobble it twice); a repeat restarts
        // from the top (a new play). The next progress sample tells them apart (onProgress).
        if (track_.durationMs <= 0 && t.durationMs > 0) {
            track_.durationMs = t.durationMs;
            listen_.learnDuration(t.durationMs);
        }
        restartCheck_ = true;
        restartWallMs_ = nowMonotonic();
        return;
    }
    startPlay(t, nowMonotonic());
}

void Scrobbler::startPlay(const ScrobbleTrack& t, int64_t wallMs) {
    active_ = true;
    playStarted_ = false;
    restartCheck_ = false;
    track_ = t;
    startedAtUnix_ = 0;
    lastPositionMs_ = 0;
    listen_.reset(t.durationMs, wallMs);
}

void Scrobbler::onStopped() {
    active_ = false;
    playStarted_ = false;
    restartCheck_ = false;
}

void Scrobbler::onProgress(int64_t positionMs, bool playing, int64_t durationMs) {
    if (!active_) return;
    try {
        if (restartCheck_) {
            restartCheck_ = false;
            // Jumped back to (near) the top after the same id started again: a repeat -> a new play.
            if (positionMs + 10'000 < lastPositionMs_) startPlay(ScrobbleTrack(track_), restartWallMs_);
        }
        lastPositionMs_ = positionMs;
        if (durationMs > 0 && track_.durationMs <= 0) {
            track_.durationMs = durationMs;
            listen_.learnDuration(durationMs);
        }
        if (playing && !playStarted_) {
            playStarted_ = true;
            // Started from (near) the top: back-date to the real start; otherwise (resumed session) "now".
            startedAtUnix_ = unixNow() - (positionMs > 0 && positionMs < 10'000 ? positionMs / 1000 : 0);
            if (enabled) sendNowPlaying();
        }
        if (listen_.update(positionMs, playing, nowMonotonic()) && enabled) {
            if (startedAtUnix_ == 0) startedAtUnix_ = unixNow();
            ST_LOG_INFO(kTag, "scrobble: {} - {} (listened {} s)", track_.artist, track_.title, listen_.listenedMs() / 1000);
            submitScrobble();
            if (onScrobble) onScrobble(track_, startedAtUnix_);
        }
    } catch (const std::exception& e) {
        ST_LOG_WARN(kTag, "progress: {}", e.what());
    }
}

void Scrobbler::sendNowPlaying() {
    if (!validTrack(track_)) return;
    if (lastfmConnected()) {
        Params p{{"method", "track.updateNowPlaying"}, {"api_key", lfmKey_}, {"sk", lfmSession_}};
        addTrackParams(p, track_, {});
        const std::string secret = lfmSecret_;
        const uint64_t gen = lfmGen_;
        async(
            Priority::Low, life_.ref(), [p = std::move(p), secret] { return lastfmCall(p, secret); },
            [this, gen](Result<LfmReply> r) {
                if (gen != lfmGen_) return;
                if (!r) return ST_LOG_WARN(kTag, "last.fm now playing failed: {}", r.errorMessage());
                if (r->error) {
                    ST_LOG_WARN(kTag, "last.fm now playing error {}: {}", r->error, r->message);
                    if (r->error == 9) dropLastfmSession("session key rejected");
                    return;
                }
                flushLastfm();   // a submission went through: retry anything still queued
            });
    }
    if (listenbrainzConnected()) {
        const json body = {{"listen_type", "playing_now"}, {"payload", json::array({lbListen(track_, 0)})}};
        const std::string token = lbToken_;
        const uint64_t gen = lbGen_;
        async(
            Priority::Low, life_.ref(), [token, b = dumpJson(body)] { return lbSubmit(token, b); },
            [this, gen](Result<LbReply> r) {
                if (gen != lbGen_) return;
                if (!r) return ST_LOG_WARN(kTag, "listenbrainz playing_now failed: {}", r.errorMessage());
                if (r->http == 401) return dropListenbrainzToken("token rejected (401)");
                if (r->http < 200 || r->http >= 300)
                    return ST_LOG_WARN(kTag, "listenbrainz playing_now HTTP {}: {}", r->http, r->message);
                flushListenbrainz();
            });
    }
}

void Scrobbler::enqueue(std::deque<Pending>& q, Pending p) {
    q.push_back(std::move(p));
    while (q.size() > kMaxQueue) {
        q.pop_front();
        ST_LOG_WARN(kTag, "retry queue full; dropped the oldest scrobble");
    }
}

void Scrobbler::submitScrobble() {
    if (!validTrack(track_)) {
        ST_LOG_DEBUG(kTag, "not scrobbling a track without title/artist");
        return;
    }
    Pending p{0, track_, startedAtUnix_};
    if (lastfmConnected()) {
        p.seq = ++seq_;
        enqueue(lfmQueue_, p);
        flushLastfm();
    }
    if (listenbrainzConnected()) {
        p.seq = ++seq_;
        enqueue(lbQueue_, p);
        flushListenbrainz();
    }
}

void Scrobbler::flushLastfm() {
    if (lfmBusy_ || lfmQueue_.empty() || !lastfmConnected()) return;
    const size_t n = std::min(lfmQueue_.size(), kLastfmBatch);
    Params p{{"method", "track.scrobble"}, {"api_key", lfmKey_}, {"sk", lfmSession_}};
    std::vector<uint64_t> seqs;
    seqs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const Pending& e = lfmQueue_[i];
        const std::string sfx = n == 1 ? std::string{} : "[" + std::to_string(i) + "]";
        addTrackParams(p, e.track, sfx);
        p.emplace_back("timestamp" + sfx, std::to_string(e.timestamp));
        seqs.push_back(e.seq);
    }
    lfmBusy_ = true;
    const std::string secret = lfmSecret_;
    const uint64_t gen = lfmGen_;
    async(
        Priority::Low, life_.ref(), [p = std::move(p), secret] { return lastfmCall(p, secret); },
        [this, gen, seqs](Result<LfmReply> r) {
            lfmBusy_ = false;
            if (gen != lfmGen_) return flushLastfm();   // account changed meanwhile: the queue was reset
            bool remove = false;
            bool ok = false;
            if (!r) {
                ST_LOG_WARN(kTag, "last.fm scrobble failed ({} queued for retry): {}", lfmQueue_.size(), r.errorMessage());
            } else if (r->error == 0) {
                ok = remove = true;
                int accepted = -1, ignored = -1;
                if (const auto s = r->body.find("scrobbles"); s != r->body.end() && s->is_object()) {
                    if (const auto a = s->find("@attr"); a != s->end() && a->is_object()) {
                        if (const auto v = a->find("accepted"); v != a->end() && v->is_number_integer()) accepted = v->get<int>();
                        if (const auto v = a->find("ignored"); v != a->end() && v->is_number_integer()) ignored = v->get<int>();
                    }
                }
                ST_LOG_INFO(kTag, "last.fm scrobbled {} (accepted {}, ignored {})", seqs.size(), accepted, ignored);
            } else {
                ST_LOG_WARN(kTag, "last.fm scrobble error {}: {}", r->error, r->message);
                if (r->error == 9) dropLastfmSession("session key rejected");
                else if (r->error == 6) remove = true;   // invalid parameters: retrying cannot help
            }
            if (remove)
                std::erase_if(lfmQueue_, [&](const Pending& e) { return std::find(seqs.begin(), seqs.end(), e.seq) != seqs.end(); });
            if (ok) flushLastfm();   // more than one batch was waiting
        });
}

void Scrobbler::flushListenbrainz() {
    if (lbBusy_ || lbQueue_.empty() || !listenbrainzConnected()) return;
    const size_t n = std::min(lbQueue_.size(), kLbBatch);
    json payload = json::array();
    std::vector<uint64_t> seqs;
    seqs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        payload.push_back(lbListen(lbQueue_[i].track, lbQueue_[i].timestamp));
        seqs.push_back(lbQueue_[i].seq);
    }
    const json body = {{"listen_type", n == 1 ? "single" : "import"}, {"payload", std::move(payload)}};
    lbBusy_ = true;
    const std::string token = lbToken_;
    const uint64_t gen = lbGen_;
    async(
        Priority::Low, life_.ref(), [token, b = dumpJson(body)] { return lbSubmit(token, b); },
        [this, gen, seqs](Result<LbReply> r) {
            lbBusy_ = false;
            if (gen != lbGen_) return flushListenbrainz();
            bool remove = false;
            bool ok = false;
            if (!r) {
                ST_LOG_WARN(kTag, "listenbrainz submit failed ({} queued for retry): {}", lbQueue_.size(), r.errorMessage());
            } else if (r->http >= 200 && r->http < 300) {
                ok = remove = true;
                ST_LOG_INFO(kTag, "listenbrainz submitted {} listen(s)", seqs.size());
            } else {
                ST_LOG_WARN(kTag, "listenbrainz submit HTTP {}: {}", r->http, r->message);
                if (r->http == 401) dropListenbrainzToken("token rejected (401)");
                else if (r->http == 400) remove = true;   // malformed listen: retrying cannot help
            }
            if (remove)
                std::erase_if(lbQueue_, [&](const Pending& e) { return std::find(seqs.begin(), seqs.end(), e.seq) != seqs.end(); });
            if (ok) flushListenbrainz();
        });
}

} // namespace st::app
