// Video mode: the controller (what exists when), painting for the views, commands, settings rows and dev hooks.
// See app/VideoMode.h.
#include "app/VideoMode.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/Components.h"
#include "app/InternetRadio.h"
#include "app/SettingsWidgets.h"
#include "app/VideoView.h"
#include "app/VideoWindow.h"
#include "app/WinShell.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/Theme.h"
#include "ui/Window.h"
#include "video/VideoPlayer.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <unordered_map>
#include <vector>

namespace st::app {

using gfx::Color;
using gfx::Rect;
namespace type = gfx::type;

namespace videomode {
namespace {

enum class Phase { Idle, Resolving, Ready, NoVideo, Failed };

struct State {
    bool on = false;
    // The picture stream of the current item.
    std::string videoId;        // what phase / stream are about ("" = nothing)
    int quality = 0;
    Phase phase = Phase::Idle;
    youtube::VideoStreamInfo stream;
    int refreshes = 0;          // new URLs after the old one was refused (403) while playing
    bool software = false;      // the GPU path failed for this video
    std::unique_ptr<video::VideoPlayer> player;
    Lifetime life;              // stream resolves in flight
    std::vector<VideoView*> views;
    std::unique_ptr<VideoWindow> window;
    // Sync statistics of the frames shown since the last log line.
    uint64_t lastShown = 0;
    int64_t errSumUs = 0, errMaxUs = 0;
    int errCount = 0, offCount = 0;
    double statsAt = 0;
    std::wstring devFile;       // SHADETUBE_VIDEO_FILE: this local video plays as every song's picture
};

State& S() {
    static State s;
    return s;
}

const catalog::Track* currentTrack() {
    auto* p = ctx().player;
    return p ? p->current() : nullptr;
}

bool songItem(const catalog::Track* t) { return t && !catalog::isPodcastId(t->id) && !radio::isStationId(t->id); }

// The YouTube video the current item streams from ("" while resolving, for files and stand-in sources).
std::string currentVideoId() {
    auto* p = ctx().player;
    if (!p || !songItem(p->current())) return {};
    const auto m = p->currentMatch();
    return m ? m->videoId : std::string();
}

bool anyViewOnScreen() {
    for (const auto* v : S().views)
        if (v->onScreen()) return true;
    return false;
}

// The song itself has started: the video's download never competes with the audio's start.
bool audioStarted() {
    auto* p = ctx().player;
    if (!p) return false;
    const auto st = p->status();
    return (st == player::Status::Playing || st == player::Status::Paused || st == player::Status::Buffering) &&
           p->clock().tag == p->clockTag();
}

void invalidateViews() {
    for (auto* v : S().views) v->invalidate();
}

void logStats();

void dropPlayer(const char* why) {
    auto& s = S();
    if (!s.player) return;
    logStats();         // what it showed since the last line
    s.player.reset();   // joins the decoder: its thread, GPU surfaces and download go now
    ST_LOG_INFO("video", "picture released ({})", why);
}

void resetStream() {
    auto& s = S();
    dropPlayer("another item");
    s.life.renew();
    s.videoId.clear();
    s.phase = Phase::Idle;
    s.stream = {};
    s.refreshes = 0;
    s.software = false;
}

void resolve(bool bypassCache) {
    auto& s = S();
    s.phase = Phase::Resolving;
    if (!s.devFile.empty()) {   // dev: the local file is every song's video
        s.stream = {};
        s.stream.mimeType = "video/mp4";
        s.phase = Phase::Ready;
        return;
    }
    auto* matcher = ctx().matcher;
    if (!matcher) return;
    const std::string id = s.videoId;
    const int quality = s.quality;
    double duration = 0;
    if (auto m = ctx().player->currentMatch(); m && m->durationSec > 0) duration = m->durationSec;
    else if (const auto* t = currentTrack()) duration = t->durationMs / 1000.0;
    async(Priority::Normal, s.life.ref(),
          [matcher, id, quality, duration, bypassCache] {
              const auto d = video::optionalDecoders();
              return matcher->videoStream(id, quality, {d.vp9, d.av1}, duration, bypassCache);
          },
          [id](Result<youtube::VideoStreamInfo> r) {
              auto& s = videomode::S();
              if (s.videoId != id) return;
              if (!r) {
                  // No picture stream this machine decodes (or YouTube refused it): the cover, said plainly.
                  bool none = false;
                  try {
                      std::rethrow_exception(r.error());
                  } catch (const youtube::NoMatchError&) {
                      none = true;
                  } catch (...) {
                  }
                  ST_LOG_WARN("video", "no picture for {}: {}", id, r.errorMessage());
                  s.phase = none ? Phase::NoVideo : Phase::Failed;
              } else {
                  s.stream = std::move(*r);
                  s.phase = s.stream.staticImage ? Phase::NoVideo : Phase::Ready;
              }
              invalidateViews();
              sync();
          });
}

void openPlayer() {
    auto& s = S();
    auto* p = ctx().player;
    video::Source src;
    if (!s.devFile.empty()) {
        src.localPath = s.devFile;
        src.mimeType = std::filesystem::path(s.devFile).extension() == L".webm" ? "video/webm" : "video/mp4";
    } else {
        src.url = s.stream.url;
        src.contentLength = s.stream.contentLength;
        src.mimeType = s.stream.mimeType;
        // A few seconds ahead of the picture: 8 s of the stream's (peak) bit rate, at least 1 MB.
        src.readAheadBytes = std::max<int64_t>(1 << 20, static_cast<int64_t>(s.stream.bitrateKbps) * 1000 / 8 * 8);
    }
    const auto clock = p->clock();
    const int64_t startUs = static_cast<int64_t>(std::max(0.0, clock.at(audio::AudioEngine::qpcNowMs())) * 1000);
    try {
        s.player = std::make_unique<video::VideoPlayer>(std::move(src), startUs, s.software);
        s.lastShown = 0;   // the statistics start over with the new player
        s.errSumUs = s.errMaxUs = 0;
        s.errCount = s.offCount = 0;
        s.statsAt = ui::frame::realNow();
        ST_LOG_INFO("video", "picture of {} from {:.1f} s ({})", s.devFile.empty() ? s.videoId : toUtf8(s.devFile), startUs / 1e6,
                    s.software ? "software" : "GPU");
    } catch (const std::exception& e) {
        ST_LOG_ERROR("video", "video player could not start: {}", e.what());
        s.player.reset();
        s.phase = Phase::Failed;
    }
}

void logStats() {
    auto& s = S();
    if (!s.player || s.errCount == 0) return;
    const auto& st = s.player->stats();
    ST_LOG_INFO("video", "sync: {} frames shown (total {}, {} dropped, {} resyncs), frame vs audio avg {:.1f} ms, worst {:.1f} ms, "
                         "{} more than a frame off; {} KB in slots, {}",
                s.errCount, st.shown, st.dropped, st.resyncs, s.errSumUs / 1000.0 / s.errCount, s.errMaxUs / 1000.0, s.offCount,
                s.player->gpuBytes() >> 10, s.player->hardware() ? "DXVA" : "software");
    s.errSumUs = s.errMaxUs = 0;
    s.errCount = s.offCount = 0;
}

void persistMode() {
    auto& st = Settings::get();
    st.videoMode = S().on;
    st.markDirty();
}

} // namespace

bool on() { return S().on; }

void setOn(bool v, bool announce) {
    auto& s = S();
    if (s.on == v) return;
    s.on = v;
    persistMode();
    ST_LOG_INFO("video", "video mode {}", v ? "on" : "off");
    if (!v) {
        resetStream();
        closeWindow();
    }
    if (announce) toast(v ? tr(L"Video modu açık") : tr(L"Video modu kapalı"));
    if (ctx().player && ctx().player->onChanged) ctx().player->onChanged();   // the player bar's button
    sync();
    invalidateViews();
    if (ctx().window) ctx().window->invalidate();
}

void toggle(bool announce) {
    const bool turnOn = !S().on;
    setOn(turnOn, announce);
    // Turned on where nothing shows it yet: Now Playing, where the cover becomes the video.
    if (turnOn && !anyViewOnScreen() && offered() && ctx().toggleNowPlaying) ctx().toggleNowPlaying(true);
}

bool offered() { return songItem(currentTrack()); }

bool enabled() {
    auto* p = ctx().player;
    if (!p || !offered()) return false;
    if (p->status() == player::Status::Resolving) return true;   // not known yet
    return !currentVideoId().empty();
}

bool showing() { return S().on && offered(); }

bool windowOpen() { return S().window != nullptr; }

void openWindow() {
    auto& s = S();
    if (!s.on) setOn(true);
    if (!s.window) {
        try {
            s.window = std::make_unique<VideoWindow>(ctx().window ? ctx().window->hwnd() : nullptr);
            s.window->onClose = [] { closeWindow(); };
            winshell::setWindowIdentity(s.window->hwnd());   // groups with the app on the taskbar
        } catch (const std::exception& e) {
            ST_LOG_ERROR("video", "video window could not be opened: {}", e.what());
            s.window.reset();
            toast(tr(L"Video penceresi açılamadı."), true);
            return;
        }
    }
    s.window->show();
    sync();
}

void closeWindow() {
    auto& s = S();
    if (!s.window) return;
    s.window->savePlacement();
    winshell::clearWindowIdentity(s.window->hwnd());
    // Usually inside the window's own message handler (close button / Alt+F4): destroyed once it has returned.
    std::shared_ptr<VideoWindow> dying(std::move(s.window));
    ShowWindow(dying->hwnd(), SW_HIDE);
    Dispatcher::post([dying]() mutable { dying.reset(); });
    ST_LOG_INFO("video", "video window closed");
    sync();
}

void toggleWindow() {
    if (windowOpen()) closeWindow();
    else openWindow();
}

void attach(VideoView* view) { S().views.push_back(view); }

void detach(VideoView* view) {
    std::erase(S().views, view);
    // Destroyed with its page or window: the picture may have lost its last view.
    Dispatcher::post([] { sync(); });
}

void sync() {
    auto& s = S();
    if (!s.on) {
        if (s.phase != Phase::Idle || s.player) resetStream();
        return;
    }
    const std::string id = currentVideoId();
    const int quality = Settings::get().videoQuality;
    if (id.empty()) {
        if (!s.videoId.empty()) resetStream();
        return;
    }
    if (id != s.videoId || quality != s.quality) {
        resetStream();
        s.videoId = id;
        s.quality = quality;
        resolve(false);
        return;
    }
    if (s.phase != Phase::Ready) return;
    if (!anyViewOnScreen()) {
        dropPlayer("not on screen");
        return;
    }
    if (s.player && s.player->lost()) {   // device released / lost, or no GPU conversion: made again here
        if (s.player->needsSoftware()) s.software = true;
        dropPlayer("graphics device went");
    }
    if (s.player && s.player->state() == video::DecoderState::Failed) {
        const bool expired = s.player->expired();
        dropPlayer("failed");
        if (expired && s.refreshes < 2 && s.devFile.empty()) {   // the URL was refused: a new one, like the audio's
            ++s.refreshes;
            ST_LOG_WARN("video", "picture stream of {} refused: new URL ({} of 2)", id, s.refreshes);
            resolve(true);
            return;
        }
        s.phase = Phase::Failed;
        invalidateViews();
        return;
    }
    if (!s.player && audioStarted()) {
        openPlayer();
        invalidateViews();
    }
}

namespace {

// The note shown with the cover: why there is no picture (or that it is on its way).
std::wstring note() {
    auto& s = S();
    if (!currentTrack()) return tr(L"Şu an bir şarkı çalmıyor");
    if (currentVideoId().empty()) {
        auto* p = ctx().player;
        if (p && p->status() == player::Status::Resolving) return tr(L"Video yükleniyor…");
        return tr(L"Bu şarkı YouTube'dan çalmıyor: video yok");
    }
    switch (s.phase) {
    case Phase::NoVideo: return tr(L"Bu şarkı için video yok");
    case Phase::Failed: return tr(L"Video oynatılamadı");
    default: return tr(L"Video yükleniyor…");
    }
}

// The largest rect of aspect w:h inside r, centered.
Rect fit(const Rect& r, float w, float h) {
    if (w <= 0 || h <= 0) return r;
    const float s = std::min(r.w / w, r.h / h);
    return r.center(std::round(w * s), std::round(h * s));
}

void paintPopOut(gfx::Canvas& c, const VideoView& view, const Rect& picture) {
    const float a = std::max(view.hoverAmount(), view.popOutHover());
    if (a <= 0.01f) return;
    Rect b = view.popOutRect();
    b = {std::min(b.x, picture.right() - 12 - b.w), std::max(b.y, picture.y + 12), b.w, b.h};
    c.fillCircle({b.cx(), b.cy()}, b.w * 0.5f, Color{0, 0, 0, (0.45f + 0.2f * view.popOutHover()) * a});
    c.icon("pop-out-thin", b.center(16, 16), Color{1, 1, 1, 0.92f * a});
    if (view.hoverAmount() > 0.01f && view.hoverAmount() < 0.99f) ui::frame::requestNext();
}

} // namespace

void paint(gfx::Canvas& c, const Rect& r, const VideoView& view) {
    auto& s = S();
    auto* p = ctx().player;
    const bool inline_ = view.style() == VideoView::Style::Inline;
    const auto* t = currentTrack();
    ID2D1Bitmap1* bmp = nullptr;
    if (s.player && p) {
        const auto clock = p->clock();
        const double displayMs = ui::frame::now();   // this frame's vblank: the picture is seen then
        // A new item that isn't audible yet keeps its first picture; the clock is still the old item's.
        const bool audible = clock.tag == p->clockTag();
        const int64_t clockUs = audible ? static_cast<int64_t>(std::max(0.0, clock.at(displayMs)) * 1000) : 0;
        bmp = s.player->frame(c.dc(), clockUs, audible ? clock.rate : 0.0, displayMs);
        if (s.player->lost() || s.player->state() == video::DecoderState::Failed) {
            Dispatcher::post([] { sync(); });   // made again (another device / software) or a new URL
            bmp = nullptr;
        } else {
            const auto& st = s.player->stats();
            if (st.shown != s.lastShown) {
                s.lastShown = st.shown;
                const int64_t err = st.lastErrorUs;
                s.errSumUs += std::llabs(err);
                s.errMaxUs = std::max(s.errMaxUs, std::llabs(err));
                ++s.errCount;
                // In sync: the frame starts at most a frame before the clock (it is the one showing then) and at most
                // the scheduler's 8 ms after it; 10 ms of slack on both sides.
                const int64_t frameUs = static_cast<int64_t>(1e6 / std::max(1.0, s.player->fps()));
                if (err > 10'000 || err < -(frameUs + 10'000)) ++s.offCount;
            }
            // The next frame's time; while the song plays at most 100 ms ahead even when the clock stands still (a
            // stall, a speed change rebuffering): its restart is no event the views hear about.
            double next = s.player->nextFrameInMs(clockUs, audible ? clock.rate : 0.0);
            if (p->isPlaying()) next = std::min(next, 100.0);
            if (auto* w = view.window()) w->invalidateAfter(next);
        }
    } else if (s.on && s.phase == Phase::Ready && !s.player) {
        Dispatcher::post([] { sync(); });   // on screen again: the picture comes back at the audio's position
    }

    if (bmp) {
        const D2D1_RECT_F src = s.player->sourceRect();
        const Rect dst = fit(r, src.right - src.left, src.bottom - src.top);
        if (inline_) c.pushRoundedClip(dst, 2);
        // Smaller than the picture (Now Playing): multi-sample linear, which needs no pre-scaled copy (high-quality cubic
        // keeps a downscaled intermediate per frame in Direct2D's cache); larger (the window, full screen): cubic.
        const bool down = dst.w * c.scale() < src.right - src.left;
        c.dc()->DrawBitmap(bmp, dst.d2d(), 1.f, down ? D2D1_INTERPOLATION_MODE_MULTI_SAMPLE_LINEAR : D2D1_INTERPOLATION_MODE_CUBIC, &src);
        if (inline_) {
            c.popLayer();
            paintPopOut(c, view, dst);
        }
        return;
    }
    // No picture (yet): the cover, with a short note.
    const std::wstring text = toUpperTr(note());
    if (inline_) {
        if (t) {
            c.shadow(r, 2, 40, 0, gfx::accent().glow);
            drawArtwork(c, t->album.images, r, 2);
        }
        auto layout = gfx::makeLayout(text, type::monoLabel, r.w);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const float pw = std::ceil(m.widthIncludingTrailingWhitespace) + 20;
        const Rect pill{r.x + 12, r.bottom() - 12 - 24, std::min(pw, r.w - 24), 24};
        c.fillPill(pill, Color{0, 0, 0, 0.55f});
        c.text(text, type::monoLabel, pill.inset(10, 0), Color{1, 1, 1, 0.88f}, gfx::TextAlign::Leading, gfx::VAlign::Center);
        paintPopOut(c, view, r);
    } else {
        const float side = std::round(std::min(r.w, r.h) * 0.42f);
        const Rect art = r.center(side, side).offset(0, -18);
        if (t) drawArtwork(c, t->album.images, art, 2);
        c.text(text, type::monoLabel, {r.x + 16, art.bottom() + 18, r.w - 32, 16}, Color{1, 1, 1, 0.7f}, gfx::TextAlign::Center);
    }
}

} // namespace videomode

// ---- Ayarlar › Oynatma ---------------------------------------------------------------------------------------------

void buildVideoRows(ui::Column* c, const std::function<void()>&) {
    const auto& st = Settings::get();
    auto* row = c->add<SettingRow>(tr(L"Video kalitesi"),
                                   tr(L"Video modunda müzik videosunun en yüksek çözünürlüğü. Ses aynı kalır; video sessizdir, "
                                      L"sesi izler ve yalnızca birkaç saniye önden indirilir."));
    const int sel = st.videoQuality == 480 ? 0 : st.videoQuality == 1080 ? 2 : 1;
    auto* seg = row->control<Segmented>(200.f, std::vector<std::wstring>{L"480p", L"720p", L"1080p"}, sel);
    seg->onChange = [](int i) {
        Settings::get().videoQuality = i == 0 ? 480 : i == 2 ? 1080 : 720;
        Settings::get().markDirty();
        videomode::sync();   // the picture comes again at the new size
    };
    settingsToggle(c, tr(L"Video modunu hatırla"), tr(L"Video modu açıkken kapattıysan ShadeTube bir sonraki açılışta da onunla başlar."),
                   st.rememberVideoMode, [](bool v) {
                       Settings::get().rememberVideoMode = v;
                       Settings::get().videoMode = videomode::on();
                   });
}

// ---- startup -------------------------------------------------------------------------------------------------------

namespace {

// Dev (SHADETUBE_VIDEO_DEMO): "<action>@<ms>[=<arg>];..." run that long after startup: on, off, window, close,
// fullscreen, ontop, nowplaying, seek=<s>, speed=<x>, pause, play, quality=<rows>, minimize, restore, log, devicelost
// (the device recreated as after a loss), idlerelease (the idle graphics release), shot=<png> (the video window),
// mainshot=<png> (the main window), quit.
void startDemo() {
    wchar_t spec[2048] = {};
    if (!GetEnvironmentVariableW(L"SHADETUBE_VIDEO_DEMO", spec, 2048)) return;
    struct Step {
        std::wstring action, arg;
    };
    static std::unordered_map<UINT_PTR, Step> steps;
    const std::wstring all = spec;
    for (size_t start = 0; start < all.size();) {
        const size_t end = std::min(all.find(L';', start), all.size());
        const std::wstring s = all.substr(start, end - start);
        start = end + 1;
        const size_t at = s.find(L'@');
        if (at == std::wstring::npos) continue;
        const size_t eq = s.find(L'=', at);
        Step step{s.substr(0, at), eq == std::wstring::npos ? std::wstring() : s.substr(eq + 1)};
        const UINT ms = static_cast<UINT>(std::max(1, _wtoi(s.c_str() + at + 1)));
        const UINT_PTR id = SetTimer(nullptr, 0, ms, [](HWND, UINT, UINT_PTR timer, DWORD) {
            KillTimer(nullptr, timer);
            const auto it = steps.find(timer);
            if (it == steps.end()) return;
            const Step st = it->second;
            steps.erase(it);
            ST_LOG_INFO("video", "video demo: {} {}", toUtf8(st.action), toUtf8(st.arg));
            auto* p = ctx().player;
            auto& s = videomode::S();
            const auto& a = st.action;
            if (a == L"on") videomode::setOn(true);
            else if (a == L"off") videomode::setOn(false);
            else if (a == L"window") videomode::openWindow();
            else if (a == L"close") videomode::closeWindow();
            else if (a == L"fullscreen" && s.window) s.window->toggleFullscreen();
            else if (a == L"ontop" && s.window) s.window->setOnTop(!s.window->onTop());
            else if (a == L"nowplaying" && ctx().toggleNowPlaying) ctx().toggleNowPlaying(true);
            else if (a == L"seek" && p) p->seek(static_cast<int64_t>(_wtof(st.arg.c_str()) * 1000));
            else if (a == L"speed" && p) p->setSpeed(static_cast<float>(_wtof(st.arg.c_str())));
            else if (a == L"pause" && p) p->pause();
            else if (a == L"play" && p) p->play();
            else if (a == L"quality") {
                Settings::get().videoQuality = _wtoi(st.arg.c_str());
                videomode::sync();
            } else if (a == L"minimize" && ctx().window) ShowWindow(ctx().window->hwnd(), SW_MINIMIZE);
            else if (a == L"restore" && ctx().window) ShowWindow(ctx().window->hwnd(), SW_RESTORE);
            else if (a == L"log") videomode::logStats();
            else if (a == L"devicelost") gfx::Device::get().recreate();   // as after D2DERR_RECREATE_TARGET
            else if (a == L"idlerelease") gfx::Device::get().release();   // as App::manageMemory after 30 s off screen
            else if (a == L"shot" && s.window) s.window->window()->renderToPng(st.arg);
            else if (a == L"mainshot" && ctx().window) ctx().window->renderToPng(st.arg);
            else if (a == L"quit" && ctx().quitApp) ctx().quitApp();
        });
        steps[id] = std::move(step);
    }
}

} // namespace

void initVideoMode() {
    auto& s = videomode::S();
    wchar_t file[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"SHADETUBE_VIDEO_FILE", file, MAX_PATH)) {
        s.devFile = file;
        ST_LOG_WARN("video", "SHADETUBE_VIDEO_FILE: {} plays as every song's video", toUtf8(s.devFile));
    }
    const auto& st = Settings::get();
    s.on = st.rememberVideoMode && st.videoMode;
    auto& c = ctx();
    c.playerChangedHooks.push_back([] {
        videomode::sync();
        if (auto& w = videomode::S().window) w->sync();
    });
    c.housekeepingHooks.push_back([] {
        videomode::sync();
        auto& s = videomode::S();
        const double now = ui::frame::realNow();
        if (s.player && now - s.statsAt >= 10'000) {
            s.statsAt = now;
            videomode::logStats();
        }
    });
    c.persistHooks.push_back([] {
        // At exit (and the end of the Windows session): the window and the picture go while the device still exists.
        auto& s = videomode::S();
        if (s.window) {
            s.window->savePlacement();
            winshell::clearWindowIdentity(s.window->hwnd());
            s.window.reset();
        }
        videomode::dropPlayer("exit");
        auto& set = Settings::get();
        set.videoMode = s.on;
    });
    c.extraWindows.push_back([]() -> ui::Window* {
        auto& w = videomode::S().window;
        return w ? w->window() : nullptr;
    });
    commands::setHandler("video-mode", [] {
        videomode::toggle(true);
        return true;
    });
    commands::setHandler("video-window", [] {
        videomode::toggleWindow();
        return true;
    });
    startDemo();
}

} // namespace st::app
