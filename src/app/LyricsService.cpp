#include "app/LyricsService.h"

#include "app/AppContext.h"
#include "app/InternetRadio.h"
#include "app/LyricsFullscreen.h"
#include "app/Shell.h"
#include "app/Source.h"
#include "audio/Downloader.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "player/Player.h"
#include "spotify/SpotifyApi.h"
#include "ui/Anim.h"
#include "ui/Window.h"

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace st::app {

namespace {

using K = lyrics::ProviderResult::Kind;

double g_offsetChangedAt = 0;
std::function<lyrics::Ranges()> g_extraProvider;
// Spotify answered 429 / 403: leave its lyrics alone until then (steadyMs), instead of asking again for every track.
std::atomic<int64_t> g_spotifyPausedUntil{0};

lyrics::ProviderResult spotifyLyrics(spotify::Api* api, const std::string& trackId, const YoutubeExplode::CancellationToken& ct) {
    const int64_t now = steadyMs();
    if (now < g_spotifyPausedUntil.load()) return {K::Failed, {}};
    try {
        const std::string body = api->trackLyrics(trackId, ct);
        if (body.empty()) return {K::NotFound, {}};
        auto l = lyrics::parseSpotify(body);
        if (!l) return {K::NotFound, {}};
        return {K::Found, std::move(*l)};
    } catch (const spotify::ApiError& e) {
        if (e.status == 429 || e.status == 403) {
            g_spotifyPausedUntil = now + (e.status == 429 ? 10 : 30) * 60 * 1000;
            ST_LOG_WARN("lyrics", "spotify lyrics paused after HTTP {}", e.status);
        }
        return {K::Failed, {}};
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyrics", "spotify lyrics failed: {}", e.what());
        return {K::Failed, {}};
    }
}

// The file a track plays from (a download or a local file), "" when it streams.
std::filesystem::path audioPathFor(const catalog::Track& t) {
    auto* p = ctx().player;
    if (!p || !p->localFileFor) return {};
    const std::wstring path = p->localFileFor(t.id);
    if (path.empty() || path[0] == L'<') return {};   // "<missing local file: ...>" (LocalFilesPage)
    return path;
}

bool nowPlayingOpen() {
    auto* shell = ctx().window ? dynamic_cast<Shell*>(ctx().window->root()) : nullptr;
    return shell && shell->nowPlaying();
}

} // namespace

lyrics::Query lyricsQueryFor(const catalog::Track& t) {
    lyrics::Query q;
    q.cacheKey = t.id.empty() ? t.name : t.id;
    q.title = t.name;
    q.artist = t.artists.empty() ? "" : t.artists[0].name;
    q.album = t.album.name;
    q.durationSec = t.durationMs / 1000;
    q.audioPath = audioPathFor(t);
    // Spotify's own lyrics (Musixmatch and others) for its tracks while logged in, when LRCLIB has no synced ones.
    if (auto* api = source::activeApi(); api && t.id.rfind("spotify:track:", 0) == 0) {
        q.fallbackName = lyrics::kSourceSpotify;
        q.fallback = [api, id = t.id](const YoutubeExplode::CancellationToken& ct) { return spotifyLyrics(api, id, ct); };
    }
    return q;
}

void setLyricsExtraProvider(std::function<lyrics::Ranges()> provider) { g_extraProvider = std::move(provider); }

int lyricsClockMs(const std::string& trackId) {
    const auto* p = ctx().player;
    if (!p) return 0;
    const int64_t pos = p->positionMs();
    const int64_t song = g_extraProvider ? lyrics::songTime(pos, g_extraProvider()) : pos;
    return static_cast<int>(song) - lyricsOffset(trackId);
}

void seekToLyricsTime(const std::string& trackId, int lyricsMs) {
    auto* p = ctx().player;
    if (!p) return;
    const int64_t song = std::max<int64_t>(0, int64_t{lyricsMs} + lyricsOffset(trackId));
    p->seek(g_extraProvider ? lyrics::mediaTime(song, g_extraProvider()) : song);
}

int lyricsOffset(const std::string& trackId) { return trackId.empty() ? 0 : lyrics::Offsets::get().offsetMs(trackId); }

void setLyricsOffset(const std::string& trackId, int ms) {
    if (trackId.empty()) return;
    lyrics::Offsets::get().set(trackId, ms);
    g_offsetChangedAt = ui::frame::realNow();
    if (ctx().window) ctx().window->invalidate();
}

double lyricsOffsetChangedAt() { return g_offsetChangedAt; }

std::wstring formatLyricsOffset(int ms) {
    if (ms == 0) return i18n::format(tr(L"{} sn"), {L"0"});
    const int a = std::abs(ms);
    std::wstring v = std::to_wstring(a / 1000);
    if (int frac = a % 1000 / 10; frac > 0) {   // hundredths, trailing zero dropped: 0,5 / 0,25
        std::wstring digits = frac < 10 ? L"0" + std::to_wstring(frac) : std::to_wstring(frac);
        if (digits.back() == L'0') digits.pop_back();
        v += i18n::decimalSeparator() + digits;
    }
    return i18n::format(tr(L"{} sn"), {(ms > 0 ? L"+" : L"−") + v});
}

std::wstring lyricsSourceLabel(const std::string& source) {
    if (source == lyrics::kSourceSpotify) return L"SPOTIFY";
    if (source == lyrics::kSourceFile) return toUpperTr(tr(L".lrc dosyası"));
    if (source == lyrics::kSourceTag) return toUpperTr(tr(L"Dosya etiketi"));
    return L"LRCLIB";
}

// ---- Downloads -------------------------------------------------------------------------------------------------

std::optional<lyrics::Query> downloadLyricsQuery(const catalog::Track& t) {
    const auto& s = Settings::get();
    if (!s.lyricsEnabled || !s.lyricsInDownloads || radio::isStationId(t.id)) return std::nullopt;
    lyrics::Query q = lyricsQueryFor(t);
    q.audioPath.clear();   // the file doesn't exist yet
    return q;
}

std::optional<lyrics::Lyrics> addDownloadLyrics(audio::DownloadRequest& req, const std::optional<lyrics::Query>& q) {
    if (!q) return std::nullopt;
    std::optional<lyrics::Lyrics> l;
    try {
        l = lyrics::fetch(*q);
    } catch (const std::exception& e) {
        ST_LOG_WARN("lyrics", "lyrics for download failed: {}", e.what());
    }
    if (!l || l->instrumental || l->lines.empty()) return std::nullopt;
    req.lyricsText = lyrics::toPlainText(*l);
    if (l->synced)
        for (const auto& line : l->lines) req.syncedLyrics.emplace_back(line.timeMs, line.text);
    return l;
}

bool writeLyricsSidecar(const std::wstring& audioPath, const lyrics::Lyrics& l, const catalog::Track& t) {
    if (!l.synced || l.lines.empty()) return false;
    const std::string text = "\xEF\xBB\xBF" + lyrics::toLrc(l, t.name, t.artistLine(), t.album.name);
    const std::filesystem::path path = lyrics::sidecarPath(audioPath);
    auto tmp = path;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

// ---- Feature -----------------------------------------------------------------------------------------------------

void initLyrics() {
    // Shortcut / button: shift the playing track's lyrics. With no lyrics view on screen, a toast tells the new value.
    ctx().lyricsOffsetBy = [](int ms) {
        const auto* t = ctx().player ? ctx().player->current() : nullptr;
        if (!t || radio::isStationId(t->id)) return;
        const std::string id = t->id;
        setLyricsOffset(id, lyricsOffset(id) + ms);
        if (!LyricsFullscreen::isOpen() && !nowPlayingOpen())
            toast(i18n::format(tr(L"Söz zamanlaması: {}"), {formatLyricsOffset(lyricsOffset(id))}));
    };
    ctx().toggleLyricsFullscreen = [] { LyricsFullscreen::toggle(); };
    ctx().trackChangedHooks.push_back([](const catalog::Track&) { LyricsFullscreen::trackChanged(); });
}

} // namespace st::app
