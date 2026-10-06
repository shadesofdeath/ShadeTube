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

#include <algorithm>
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

// `language`: the translation language, which the translations Spotify sends along follow.
lyrics::ProviderResult spotifyLyrics(spotify::Api* api, const std::string& trackId, const std::string& language,
                                     const YoutubeExplode::CancellationToken& ct) {
    const int64_t now = steadyMs();
    if (now < g_spotifyPausedUntil.load()) return {K::Failed, {}};
    try {
        const std::string body = api->trackLyrics(trackId, ct, language);
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
        q.fallback = [api, id = t.id, lang = lyricsTranslationTarget()](const YoutubeExplode::CancellationToken& ct) {
            return spotifyLyrics(api, id, lang, ct);
        };
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

// ---- Translation -------------------------------------------------------------------------------------------------

namespace {

// The machine translator behind the lyrics translation: another lyrics::Translator plugs in here.
std::unique_ptr<lyrics::Translator> makeTranslator() { return std::make_unique<lyrics::GoogleTranslator>(); }

bool anyTranslatable(const lyrics::Lyrics& l) {
    return !l.instrumental &&
           std::any_of(l.lines.begin(), l.lines.end(), [](const lyrics::Line& x) { return lyrics::translatable(x.text); });
}

} // namespace

std::string lyricsTranslationTarget() {
    const std::string& to = Settings::get().lyricsTranslateTo;
    return to.empty() ? std::string(i18n::code()) : to;
}

std::wstring lyricsTranslationLanguageName(const std::string& code) {
    for (const auto& l : lyrics::targetLanguages())
        if (code == l.code) return l.name;
    return toWide(code);
}

bool LyricsTranslation::on() const { return Settings::get().lyricsTranslate; }

const std::string& LyricsTranslation::line(size_t i) const {
    static const std::string none;
    return state_ == State::Ready && i < result_.lines.size() ? result_.lines[i] : none;
}

void LyricsTranslation::changed() {
    if (onChange) onChange();
}

void LyricsTranslation::clear() {
    life_.renew();
    lyrics_.reset();
    key_.clear();
    result_ = {};
    state_ = State::None;
    on_ = on();
}

void LyricsTranslation::setLyrics(const lyrics::Lyrics& l, const std::string& trackKey) {
    life_.renew();
    result_ = {};
    lyrics_ = std::make_shared<const lyrics::Lyrics>(l);   // the workers' copy
    key_ = trackKey;
    target_ = lyricsTranslationTarget();
    on_ = on();
    if (!anyTranslatable(l)) {
        state_ = State::None;
        changed();
        return;
    }
    // Is it another language? A cached translation knows (and is shown at once), else the lyrics' source says, else
    // Windows' detection guesses. Unknown: offered, the translator then tells.
    state_ = State::Detecting;
    struct Found {
        bool offer = true;
        std::optional<lyrics::Translation> ready;
    };
    async(Priority::Normal, life_.ref(),
          [l = lyrics_, key = key_, target = target_] {
              Found f;
              if (auto cached = lyrics::cachedTranslation(*l, key, target)) {
                  f.offer = !cached->sameLanguage;
                  if (f.offer) f.ready = std::move(cached);
                  return f;
              }
              const std::string lang = lyrics::detectLanguage(*l);
              f.offer = lang.empty() || !lyrics::sameLanguage(lang, target);
              return f;
          },
          [this](Result<Found> r) {
              state_ = r && !r->offer ? State::None : State::Offered;
              if (r && r->ready) {
                  result_ = std::move(*r->ready);
                  state_ = State::Ready;
              }
              if (state_ == State::Offered && on()) start();
              else changed();
          });
}

void LyricsTranslation::start() {
    if (!lyrics_) return;
    state_ = State::Loading;
    changed();
    async(Priority::Normal, life_.ref(),
          [l = lyrics_, key = key_, target = target_] {
              const auto translator = makeTranslator();
              return lyrics::translate(*l, key, target, *translator);
          },
          [this](Result<std::optional<lyrics::Translation>> r) {
              if (!r || !*r) {
                  state_ = State::Failed;
              } else if ((*r)->sameLanguage) {
                  state_ = State::None;   // the translator saw the target language: nothing to offer (cached)
              } else {
                  result_ = std::move(**r);
                  state_ = State::Ready;
              }
              changed();
          });
}

void LyricsTranslation::sync() {
    if (!lyrics_) return;
    if (lyricsTranslationTarget() != target_) {   // another language picked in Ayarlar: start over
        const auto l = lyrics_;
        const std::string key = key_;
        setLyrics(*l, key);
        return;
    }
    const bool o = on();
    if (o == on_) return;
    on_ = o;
    if (o && (state_ == State::Offered || state_ == State::Failed)) {
        start();
        return;
    }
    if (!o && state_ == State::Failed) state_ = State::Offered;
    changed();
}

void LyricsTranslation::toggle() {
    auto& s = Settings::get();
    s.lyricsTranslate = !s.lyricsTranslate;
    s.markDirty();
    sync();
}

std::wstring LyricsTranslation::note() const {
    if (!on()) return {};
    switch (state_) {
    case State::Loading: return toUpperTr(tr(L"Çevriliyor…"));
    case State::Failed: return toUpperTr(tr(L"Çeviri alınamadı"));
    case State::Ready: {
        std::wstring s = toUpperTr(tr(L"Çeviri")) + L" · " + toUpperTr(lyricsTranslationLanguageName(result_.target));
        if (result_.provider == lyrics::kTranslatorGoogle) s += L" · GOOGLE";
        else if (result_.provider == lyrics::kSourceSpotify) s += L" · SPOTIFY";
        return s;
    }
    default: return {};
    }
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
