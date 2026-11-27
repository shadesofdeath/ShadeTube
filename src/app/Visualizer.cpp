#include "app/Visualizer.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/InternetRadio.h"
#include "app/LyricsFullscreen.h"
#include "app/LyricsService.h"
#include "app/Shell.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "player/Player.h"
#include "ui/Acrylic.h"
#include "ui/Micro.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <numbers>
#include <random>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;
namespace viz = audio::viz;
using Mode = Visualizer::Mode;

namespace {

Visualizer* g_view = nullptr;

constexpr float kTopH = 64;
constexpr double kControlsIdleMs = 2500;
constexpr double kReducedFrameMs = 100;     // reduce motion: 10 frames a second
constexpr double kFrameMs = 8.0;            // at most ~125 frames a second (see paint)
constexpr double kCurtainStepMs = 1000.0 / 30;
constexpr double kTrailStepMs = 45;
constexpr double kPulseLife = 0.9;          // s
constexpr int kBands = 96, kWavePoints = 320, kTrail = 4, kMotes = 64;
constexpr float kPi = std::numbers::pi_v<float>;

std::wstring modeName(Mode m) {
    switch (m) {
    case Mode::Bars: return tr(L"Spektrum");
    case Mode::Ring: return tr(L"Halka");
    case Mode::Wave: return tr(L"Dalga");
    case Mode::Aurora: return tr(L"Aurora");
    }
    return {};
}

bool devFlag(const wchar_t* name) {
    wchar_t v[8] = {};
    return GetEnvironmentVariableW(name, v, 8) > 0 && v[0] != L'0';
}

std::wstring devValue(const wchar_t* name) {
    wchar_t v[512] = {};
    const DWORD n = GetEnvironmentVariableW(name, v, 512);
    return n > 0 && n < 512 ? std::wstring(v, n) : std::wstring();
}

// SHADETUBE_VIZ_SHOTS=<folder>;<ms>;<count>;<gap ms>: consecutive frames as PNGs once the view has been open <ms> (motion
// checks; the main window's renderToPng).
struct Shots {
    std::wstring folder;
    double startMs = 0, gapMs = 0;
    int count = 0, taken = 0;
};
Shots& shots() {
    static Shots s = [] {
        Shots r;
        const std::wstring v = devValue(L"SHADETUBE_VIZ_SHOTS");
        if (v.empty()) return r;
        std::vector<std::wstring> parts;
        size_t a = 0;
        for (size_t b; (b = v.find(L';', a)) != std::wstring::npos; a = b + 1) parts.push_back(v.substr(a, b - a));
        parts.push_back(v.substr(a));
        if (parts.size() < 4) return r;
        r.folder = parts[0];
        r.startMs = _wtof(parts[1].c_str());
        r.count = _wtoi(parts[2].c_str());
        r.gapMs = _wtof(parts[3].c_str());
        return r;
    }();
    return s;
}

// exp(-x) for x in 0..8 from a table (the curtains evaluate it ~100 000 times per render).
struct ExpTable {
    static constexpr int kN = 1024;
    static constexpr float kMax = 8.f;
    std::array<float, kN + 1> v{};
    ExpTable() {
        for (int i = 0; i <= kN; ++i) v[static_cast<size_t>(i)] = std::exp(-kMax * static_cast<float>(i) / kN);
    }
    float operator()(float x) const {
        if (x <= 0) return 1.f;
        if (x >= kMax) return 0.f;
        const float p = x * (kN / kMax);
        const int i = static_cast<int>(p);
        const float f = p - static_cast<float>(i);
        return v[static_cast<size_t>(i)] + (v[static_cast<size_t>(i) + 1] - v[static_cast<size_t>(i)]) * f;
    }
};
const ExpTable& expTable() {
    static const ExpTable t;
    return t;
}

float smooth01(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}

// A colour of the cover made into light for the curtains: its hue, saturated, at a lightness that reads on the theme's
// paper (bright on dark themes, clear washes on light ones).
Color curtainTone(Color c, bool light) {
    gfx::Hsl h = gfx::toHsl(c);
    h.s = std::clamp(std::max(h.s, light ? 0.62f : 0.5f), 0.f, 0.9f);
    h.l = light ? 0.6f : 0.64f;   // deep yellows and slates turn muddy on paper
    return gfx::fromHsl(h);
}

float hueDistance(float a, float b) {
    const float d = std::fabs(a - b);
    return std::min(d, 360.f - d);
}

} // namespace

// ---- Opening / closing -----------------------------------------------------------------------------------------

const char* Visualizer::modeId(Mode m) {
    switch (m) {
    case Mode::Bars: return "bars";
    case Mode::Ring: return "ring";
    case Mode::Wave: return "wave";
    case Mode::Aurora: return "aurora";
    }
    return "ring";
}

Visualizer::Mode Visualizer::savedMode() {
    std::string id = Settings::get().visualizerMode;
    if (const std::wstring dev = devValue(L"SHADETUBE_VIZ_MODE"); !dev.empty()) id = toUtf8(dev);
    for (int i = 0; i < kModes; ++i)
        if (id == modeId(static_cast<Mode>(i))) return static_cast<Mode>(i);
    return Mode::Ring;
}

void Visualizer::toggle() {
    if (g_view) {
        g_view->close();
        return;
    }
    auto* w = ctx().window;
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!w) return;
    if (!t) {
        toast(tr(L"Görselleştirici için önce bir şey çal"));
        return;
    }
    if (LyricsFullscreen::isOpen()) LyricsFullscreen::toggle();   // one full-window view at a time
    w->pushOverlay(std::make_unique<Visualizer>(), true);
}

bool Visualizer::isOpen() { return g_view != nullptr; }

void Visualizer::trackChanged() {
    if (g_view) g_view->syncTrack();
}

Visualizer::Visualizer()
    : title_({}, type::sectionTitle.withSize(19)), artist_({}, type::bodyRegular), time_({}, type::monoDuration),
      label_({}, type::monoLabel) {
    g_view = this;
    drag_ = add<ui::Widget>();
    drag_->isDragRegion = true;
    for (int i = 0; i < kModes; ++i) {
        const Mode m = static_cast<Mode>(i);
        auto* b = add<ui::Button>(ui::ButtonKind::Chip, modeName(m));
        b->setTooltip(modeName(m) + L"\t" + std::to_wstring(i + 1));
        b->onClick = [this, m] {
            setMode(m);
            showControls();
        };
        modeBtns_[static_cast<size_t>(i)] = b;
    }
    lyricsBtn_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "lyrics");
    lyricsBtn_->onClick = [this] { setLyricsShown(!Settings::get().visualizerLyrics); };
    fullBtn_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "expand");
    fullBtn_->onClick = [this] {
        auto* w = window();
        setFullscreen(!(w && w->isFullscreen()));
    };
    close_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "close");
    close_->setTooltip(tr(L"Kapat") + std::wstring(L"\tEsc"));
    close_->onClick = [this] { close(); };
    gfx::TextOptions center;
    center.align = gfx::TextAlign::Center;
    lyric_.setOptions(center);
    lyricOld_.setOptions(center);

    mode_ = from_ = savedMode();
    analyzer_.configure(kBands, kWavePoints);
    std::mt19937 rng(0xA0A0);
    std::uniform_real_distribution<float> u(0.f, 1.f);
    motes_.resize(kMotes);
    for (auto& m : motes_) m = {u(rng), 0.08f + 0.75f * u(rng), 0.006f + 0.012f * u(rng), 0.8f + 1.6f * u(rng), u(rng) * 6.283f};
    open_.to(1, ui::overlay::duration(ui::motion::medium), ui::Ease::Decelerate);
    controls_.snap(1);
    lastMove_ = ui::frame::realNow();
    statsAt_ = lastMove_;
    syncTrack();
    syncControls();
    ST_LOG_INFO("visualizer", "opened ({})", modeId(mode_));
}

Visualizer::~Visualizer() {
    if (g_view == this) g_view = nullptr;
}

void Visualizer::close() {
    if (closing_) return;
    closing_ = true;
    if (g_view == this) g_view = nullptr;
    life_.renew();
    if (auto* w = window()) {
        if (w->isFullscreen()) w->setFullscreen(false);
        exit_.snap(1);
        exit_.to(0, ui::overlay::duration(140), ui::Ease::Accelerate);   // the dialogs' close
        w->removeOverlay(this);
    }
}

bool Visualizer::coversWindow() const { return !closing_ && open_.value() >= 0.999f; }

void Visualizer::setMode(Mode m) {
    if (m == mode_) return;
    // From what shows now (mid cross-fade: the incoming mode) to the new one.
    from_ = modeMix_.value() < 0.5f ? from_ : mode_;
    mode_ = m;
    modeMix_.snap(0);
    modeMix_.to(1, ui::motion::micro() ? ui::motion::medium : 0, ui::Ease::Standard);
    pulses_.clear();
    Settings::get().visualizerMode = modeId(m);
    Settings::get().markDirty();
    syncControls();
    invalidate();
}

void Visualizer::setLyricsShown(bool on) {
    Settings::get().visualizerLyrics = on;
    Settings::get().markDirty();
    if (on && lyrics_.lines.empty()) loadLyrics();
    syncControls();
    showControls();
}

void Visualizer::setFullscreen(bool on) {
    if (auto* w = window()) w->setFullscreen(on);
    syncControls();
    showControls();
}

void Visualizer::syncControls() {
    for (int i = 0; i < kModes; ++i) modeBtns_[static_cast<size_t>(i)]->setActive(static_cast<int>(mode_) == i);
    const bool lyricsOn = Settings::get().visualizerLyrics;
    lyricsBtn_->setActive(lyricsOn);
    lyricsBtn_->setTooltip((lyricsOn ? tr(L"Söz satırını gizle") : tr(L"Söz satırını göster")) + std::wstring(L"\tL"));
    const bool full = window() && window()->isFullscreen();
    fullBtn_->setIcon(full ? "collapse" : "expand");
    fullBtn_->setTooltip((full ? tr(L"Tam ekrandan çık") : tr(L"Tam ekran")) + std::wstring(L"\tF"));
    drag_->setVisible(!full);
    requestLayout();
    invalidate();
}

void Visualizer::showControls() {
    lastMove_ = ui::frame::realNow();
    if (controls_.target() < 1) controls_.to(1, ui::motion::fast);
    if (auto* w = window()) w->invalidateAfter(kControlsIdleMs + 50);
    invalidate();
}

// ---- The track and its lyric line ---------------------------------------------------------------------------------

void Visualizer::syncTrack() {
    const auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    const std::string id = t ? t->id : std::string();
    title_.setText(t ? toWide(t->name) : std::wstring());
    const bool station = t && radio::isStationId(t->id);
    artist_.setText(!t ? std::wstring() : station && !p->liveTitle().empty() ? p->liveTitle() : toWide(t->artistLine()));
    if (id == trackId_) return;
    trackId_ = id;
    loadLyrics();
    invalidate();
}

void Visualizer::loadLyrics() {
    life_.renew();
    lyrics_ = {};
    synced_ = false;
    activeLine_ = -1;
    lyric_.setText(L"");
    lyricOld_.setText(L"");
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    // Songs only (a station has no timeline, an episode no lyrics), and only when the line is shown.
    if (!t || radio::isStationId(t->id) || catalog::isPodcastId(t->id) || !Settings::get().lyricsEnabled ||
        !Settings::get().visualizerLyrics)
        return;
    async(Priority::Normal, life_.ref(), [q = lyricsQueryFor(*t)] { return lyrics::fetch(q); },
          [this](Result<std::optional<lyrics::Lyrics>> r) {
              if (!r || !*r) return;
              lyrics_ = std::move(**r);
              synced_ = lyrics_.synced && !lyrics_.instrumental && !lyrics_.lines.empty();
              invalidate();
          });
}

void Visualizer::paintLyric(Canvas& c, const Rect& r) {
    if (!Settings::get().visualizerLyrics || !synced_) return;
    const int line = lyrics::activeLine(lyrics_, lyricsClockMs(trackId_) + 150);   // the lyrics views' look-ahead
    if (line != activeLine_) {
        lyricOld_.setText(lyric_.text());
        lyric_.setText(line >= 0 && line < static_cast<int>(lyrics_.lines.size()) ? toWide(lyrics_.lines[static_cast<size_t>(line)].text)
                                                                                 : std::wstring());
        activeLine_ = line;
        lyricIn_.snap(0);
        lyricIn_.to(1, ui::motion::micro() ? ui::motion::slow : 0, ui::Ease::Decelerate);
    }
    const float in = lyricIn_;
    const Color col = colors().fgPrimary;
    const float rise = std::round(r.h * 0.18f);
    if (in < 0.999f && !lyricOld_.empty()) c.text(lyricOld_, r.offset(0, -rise * in), col.withAlpha(0.9f * (1.f - in)), gfx::VAlign::Center);
    if (!lyric_.empty()) c.text(lyric_, r.offset(0, rise * (1.f - in)), col.withAlpha(0.9f * in), gfx::VAlign::Center);
}

// ---- Layout -------------------------------------------------------------------------------------------------------

void Visualizer::layout() {
    const Rect r = rect();
    const float pad = std::round(std::clamp(r.w * 0.045f, 32.f, 72.f));
    // Top strip, right to left: close, full screen, the lyric line, then the mode chips.
    const float cy = 32, bs = 32;
    float x = r.w - pad - bs;
    close_->setRect({x, cy - bs / 2, bs, bs});
    x -= 6 + bs;
    fullBtn_->setRect({x, cy - bs / 2, bs, bs});
    x -= 6 + bs;
    lyricsBtn_->setRect({x, cy - bs / 2, bs, bs});
    x -= 20;
    for (int i = kModes - 1; i >= 0; --i) {
        auto* b = modeBtns_[static_cast<size_t>(i)];
        const float w = std::ceil(b->naturalWidth());
        x -= w;
        b->setRect({x, cy - 14, w, 28});
        x -= 6;
    }
    drag_->setRect({0, 0, std::max(0.f, x - 10), kTopH});
    // The corner (title, artist, progress), the lyric line above it, the visual over the rest.
    const float lyricSize = std::clamp(std::round(r.h * 0.03f), 20.f, 34.f);
    const gfx::TextStyle ls{lyricSize, 700, 1.15f, -0.02f};
    if (!(lyric_.style() == ls)) {
        lyric_.setStyle(ls);
        lyricOld_.setStyle(ls);
    }
    cornerRect_ = {pad, r.h - pad - 66, std::min(460.f, std::max(240.f, r.w * 0.36f)), 66};
    const float lyricH = std::round(lyricSize * 1.6f);
    lyricRect_ = {pad, cornerRect_.y - 20 - lyricH, r.w - 2 * pad, lyricH};
    const float top = kTopH + 8;
    contentRect_ = {pad, top, r.w - 2 * pad, std::max(80.f, lyricRect_.y - 12 - top)};
}

// ---- Painting -------------------------------------------------------------------------------------------------------

// The analysis of this frame: the latest samples of the tap, the frame time since the previous one.
void Visualizer::analyse(bool& live) {
    const auto* p = ctx().player;
    const double now = ui::frame::now();
    const double dt = lastFrame_ > 0 ? std::clamp((now - lastFrame_) / 1000.0, 0.0, 0.25) : 1.0 / 60;
    lastFrame_ = now;
    motion_ = ui::motion::micro();
    analyzer_.setResponse(motion_ ? 0.035f : 0.25f, motion_ ? 0.24f : 0.9f);
    audio::AudioEngine::WaveInfo info;
    // A song preview holding the current item (app/Preview) is not what plays here.
    live = p && !p->previewHoldsCurrent() && p->waveform(samples_.data(), static_cast<int>(samples_.size()), &info);
    analyzer_.update(samples_.data(), samples_.size(), info.position, info.rate, dt, live);
    if (live && motion_) drift_ += dt;
    const auto& L = analyzer_.levels();
    if (L.beats != lastBeats_) {
        lastBeats_ = L.beats;
        if (motion_ && mode_ == Mode::Ring) {
            pulses_.push_back({now / 1000.0, std::max(0.4f, L.beat)});
            if (pulses_.size() > 4) pulses_.erase(pulses_.begin());
        }
    }
    std::erase_if(pulses_, [now](const Pulse& q) { return now / 1000.0 - q.born > kPulseLife; });
    // Dalga's trail: a copy of the waveform every ~45 ms while it moves (also while a pause flattens it, so the trail
    // settles with it instead of vanishing).
    const bool resting = analyzer_.resting();
    if (motion_ && !resting && now - trailAt_ >= kTrailStepMs) {
        trailAt_ = now;
        if (trail_.size() >= kTrail) {
            auto oldest = std::move(trail_.back());
            trail_.pop_back();
            oldest = analyzer_.waveform();
            trail_.insert(trail_.begin(), std::move(oldest));
        } else {
            trail_.insert(trail_.begin(), analyzer_.waveform());
        }
    } else if (resting || !motion_) {
        trail_.clear();
    }
}

void Visualizer::paintMode(Canvas& c, Mode m, const Rect& area, float alpha) {
    if (alpha <= 0.002f) return;
    switch (m) {
    case Mode::Bars: paintBars(c, area, alpha); break;
    case Mode::Ring: paintRing(c, area, alpha); break;
    case Mode::Wave: paintWave(c, area, alpha); break;
    case Mode::Aurora: paintAurora(c, area, alpha); break;
    }
}

// Spektrum: pills standing a little above a hairline baseline, a faint reflection under it, the peaks as short
// fg.primary dashes that hold and then fall.
void Visualizer::paintBars(Canvas& c, const Rect& area, float alpha) {
    const auto& L = analyzer_.levels();
    const auto& col = colors();
    const int n = std::clamp(static_cast<int>(area.w / 15.f), 24, kBands);
    const float pitch = area.w / static_cast<float>(n);
    const float bw = std::max(2.f, std::round(pitch * 0.55f));
    const float base = std::round(area.y + area.h * 0.8f);
    const float maxH = area.h * 0.74f;
    const Color acc = accent().base;
    c.hline(area.x, area.right(), base, col.hairSubtle.mulAlpha(alpha));
    for (int i = 0; i < n; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        const float v = viz::sampleAt(L.bands, u);
        const float h = std::max(bw, v * maxH);
        const float x = std::round(area.x + static_cast<float>(i) * pitch + (pitch - bw) * 0.5f);
        c.fillRounded({x, base - 3 - h, bw, h}, bw * 0.5f, acc.withAlpha((0.45f + 0.55f * v) * alpha));
        c.fillRounded({x, base + 3, bw, std::max(1.f, h * 0.22f)}, bw * 0.5f, acc.withAlpha((0.05f + 0.07f * v) * alpha));
        const float pk = viz::sampleAt(L.peaks, u);
        if (pk > v + 0.02f) c.fillRect({x, std::round(base - 3 - pk * maxH - 5), bw, 2}, col.fgPrimary.withAlpha(0.5f * alpha));
    }
}

// The cover in a circle (Halka): cross-fades in when a new one arrives; a placeholder disc meanwhile.
void Visualizer::paintCover(Canvas& c, const Rect& r, float alpha) {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    const int px = static_cast<int>(std::ceil(r.w * c.scale()));
    const catalog::Image* img = t ? catalog::pickImage(t->album.images, px) : nullptr;
    const std::string url = img ? img->url : std::string();
    ID2D1Bitmap1* bmp = url.empty() ? nullptr : gfx::ImageCache::get().request(url, px, Priority::High);
    if (url != coverUrl_) {
        coverUrl_ = url;
        coverIn_.snap(bmp ? 1.f : 0.f);
    }
    if (bmp && coverIn_.target() < 1) coverIn_.to(1, ui::motion::micro() ? ui::motion::slow : 0);
    const float in = bmp ? coverIn_.value() : 0.f;
    const auto& col = colors();
    if (in < 0.999f) {
        c.fillCircle({r.cx(), r.cy()}, r.w * 0.5f, (gfx::isLightTheme() ? col.bgSunken : col.bgOverlay).mulAlpha(alpha));
        const float g = std::round(r.w * 0.28f);
        c.icon("album", r.center(g, g), col.fgTertiary.mulAlpha(alpha * (1.f - in)));
    }
    if (bmp && in > 0.001f) c.image(bmp, r, r.w * 0.5f, alpha * in);
    c.strokeCircle({r.cx(), r.cy()}, r.w * 0.5f, col.hairDefault.mulAlpha(alpha));
}

// Halka: the spectrum mirrored around the round cover (bass at the top), a soft glow of the accent behind it that
// swells with the bass, and a thin ring leaving it on every beat.
void Visualizer::paintRing(Canvas& c, const Rect& area, float alpha) {
    const auto& L = analyzer_.levels();
    const gfx::Point ctr{area.cx(), area.cy()};
    const float R = std::round(std::clamp(std::min(area.w, area.h) * 0.23f, 64.f, 250.f));
    const float r0 = R + std::round(R * 0.08f + 6);
    const float maxL = std::max(16.f, std::min(area.h * 0.5f - r0 - 4, R * 0.85f));
    const Color acc = accent().base;
    const float bass = motion_ ? L.bass : 0.35f;
    // Glow.
    const float glowR = r0 + maxL * 0.9f;
    c.fillRadialGradient({ctr.x - glowR, ctr.y - glowR, glowR * 2, glowR * 2}, ctr, glowR,
                         acc.withAlpha((0.08f + 0.16f * bass) * alpha), acc.withAlpha(0));
    // Beat rings.
    const double now = ui::frame::now() / 1000.0;
    for (const auto& q : pulses_) {
        const float age = static_cast<float>((now - q.born) / kPulseLife);
        if (age < 0 || age >= 1) continue;
        const float k = 1.f - age;
        c.strokeCircle(ctr, r0 + age * (maxL + 40), acc.withAlpha(q.strength * 0.5f * k * k * alpha), 1.5f);
    }
    // The spectrum: lines on both halves, the lowest bands at the top.
    const int n = std::clamp(static_cast<int>(kPi * r0 / 9.f), 32, 90);
    const float lw = std::clamp(kPi * r0 / static_cast<float>(n) * 0.42f, 1.5f, 4.f);
    // The deepest bands (below ~50 Hz: little but rumble, and the window can't tell them apart) are left out, and each
    // line takes a little of its neighbours, so the outline reads as one shape rather than a comb.
    auto at = [&](float u) { return viz::sampleAt(L.bands, 0.07f + 0.93f * std::clamp(u, 0.f, 1.f)); };
    const float du = 1.f / static_cast<float>(n);
    for (int i = 0; i < n; ++i) {
        const float u = (static_cast<float>(i) + 0.5f) / static_cast<float>(n);
        const float v = 0.5f * at(u) + 0.25f * (at(u - du) + at(u + du));
        const float len = 2.f + v * maxL;
        const float a = -kPi * 0.5f + u * kPi;   // from the top, clockwise, down to the bottom
        const Color lc = acc.withAlpha((0.4f + 0.6f * v) * alpha);
        for (float side : {1.f, -1.f}) {
            const float dx = std::cos(a) * side, dy = std::sin(a);
            c.line({ctr.x + dx * r0, ctr.y + dy * r0}, {ctr.x + dx * (r0 + len), ctr.y + dy * (r0 + len)}, lc, lw);
        }
    }
    // The cover, breathing a little with the beat.
    const float s = motion_ ? 1.f + 0.022f * L.beat : 1.f;
    const Rect cover{ctr.x - R, ctr.y - R, 2 * R, 2 * R};
    if (s > 1.0005f) c.pushScale(s, ctr);
    paintCover(c, cover, alpha);
    if (s > 1.0005f) c.popTransform();
}

// Dalga: the waveform across the view, tapered to the middle line at both ends (the seek wave's shape), its last few
// shapes trailing behind in fading strokes.
void Visualizer::paintWave(Canvas& c, const Rect& area, float alpha) {
    const auto& col = colors();
    const Color acc = accent().base;
    const float y0 = std::round(area.cy());
    const float A = area.h * 0.34f;
    c.hline(area.x, area.right(), y0, col.hairSubtle.mulAlpha(alpha));
    auto& pts = wavePts_;
    pts.resize(kWavePoints);
    // `step` 2: every second point (the faint trail strokes: half the geometry to widen per frame).
    auto shape = [&](const std::vector<float>& w, float gain, size_t step) {
        const size_t n = std::min(w.size(), pts.size());
        if (n < 2) return size_t(0);
        size_t k = 0;
        for (size_t i = 0; i < n; i += step) {
            const size_t j = std::min(i, n - 1);
            const float u = static_cast<float>(j) / static_cast<float>(n - 1);
            const float taper = std::pow(std::max(0.f, std::sin(kPi * u)), 0.75f);   // sin(pi) is a hair below 0
            pts[k++] = {area.x + u * area.w, y0 - w[j] * A * taper * gain};
        }
        if (step > 1 && (n - 1) % step != 0) pts[k++] = {area.right(), y0};   // the end on the line
        return k;
    };
    const float trailAlpha[kTrail] = {0.26f, 0.16f, 0.09f, 0.05f};
    for (int k = static_cast<int>(trail_.size()) - 1; k >= 0; --k) {
        const size_t n = shape(trail_[static_cast<size_t>(k)], 1.f - 0.04f * static_cast<float>(k + 1), 2);
        c.polyline(pts.data(), n, acc.withAlpha(trailAlpha[k] * alpha), 1.5f);
    }
    const size_t n = shape(analyzer_.waveform(), 1.f, 1);
    c.polyline(pts.data(), n, acc.withAlpha(alpha), 2.f);
}

// The curtains: for every column, three ribbons with a sharp lower edge and a long fade upwards, undulating slowly,
// streaked by slow vertical rays; each lit by its part of the spectrum (bass, mids, treble). Premultiplied BGRA.
void Visualizer::renderCurtains(Canvas& c, int w, int h) {
    const bool light = gfx::isLightTheme();
    // Colours: the accent, then the most vivid cover colours of another hue (else neighbours of the accent's).
    const Color acc = gfx::Theme::get().accentTarget();
    const float accHue = gfx::toHsl(acc).h;
    std::array<Color, 3> tone{curtainTone(acc, light), {}, {}};
    std::vector<Color> pool;
    if (const auto* pal = backdrop_.palette())
        for (int i = 0; i < cover::kPaletteSize; ++i) pool.push_back(pal->colors[static_cast<size_t>(i)]);
    std::sort(pool.begin(), pool.end(), [](Color a, Color b) { return gfx::toHsl(a).s > gfx::toHsl(b).s; });
    int filled = 1;
    for (const Color pc : pool) {
        if (filled >= 3) break;
        const gfx::Hsl ph = gfx::toHsl(pc);
        bool distinct = ph.s > 0.18f && hueDistance(ph.h, accHue) > 25.f;
        for (int k = 1; k < filled; ++k) distinct = distinct && hueDistance(ph.h, gfx::toHsl(tone[static_cast<size_t>(k)]).h) > 25.f;
        if (distinct) tone[static_cast<size_t>(filled++)] = curtainTone(pc, light);
    }
    for (; filled < 3; ++filled) {
        gfx::Hsl hh = gfx::toHsl(tone[0]);
        hh.h = std::fmod(hh.h + (filled == 1 ? 38.f : 322.f), 360.f);
        tone[static_cast<size_t>(filled)] = gfx::fromHsl(hh);
    }
    const auto& L = analyzer_.levels();
    const std::array<float, 3> energy = motion_ ? std::array<float, 3>{L.bass, L.mid, L.treble} : std::array<float, 3>{0.5f, 0.5f, 0.5f};
    struct Ribbon {
        float base, fade, amp1, f1, s1, amp2, f2, s2, r1, rs1, r2, rs2, alpha;
    };
    const float k = light ? 0.72f : 1.f;
    const Ribbon rb[3] = {
        {0.60f, 0.27f, 0.050f, 0.55f, 0.016f, 0.020f, 2.1f, 0.050f, 3.0f, 0.030f, 7.0f, 0.070f, 0.74f * k},
        {0.50f, 0.22f, 0.045f, 0.85f, -0.022f, 0.020f, 2.7f, 0.040f, 4.2f, -0.045f, 9.0f, 0.090f, 0.62f * k},
        {0.41f, 0.17f, 0.035f, 1.25f, 0.030f, 0.015f, 3.4f, 0.060f, 5.5f, 0.060f, 11.0f, -0.080f, 0.54f * k},
    };
    const float T = static_cast<float>(drift_);
    if (curtainW_ != w || curtainH_ != h) {
        curtainW_ = w;
        curtainH_ = h;
        curtainTexels_.assign(static_cast<size_t>(w) * h, 0);
    }
    const auto& ex = expTable();
    const float twoPi = 2.f * kPi;
    std::array<float, 3> yk{}, ray{}, gain{}, inv{};
    for (int x = 0; x < w; ++x) {
        const float u = static_cast<float>(x) / static_cast<float>(std::max(1, w - 1));
        for (int i = 0; i < 3; ++i) {
            const Ribbon& b = rb[i];
            yk[i] = b.base + b.amp1 * std::sin(twoPi * (b.f1 * u + b.s1 * T + 0.3f * i)) + b.amp2 * std::sin(twoPi * (b.f2 * u - b.s2 * T));
            const float r = 0.5f + 0.5f * std::sin(twoPi * (b.r1 * u + b.rs1 * T)) * std::sin(twoPi * (b.r2 * u - b.rs2 * T + 0.17f * i));
            ray[i] = 0.45f + 0.55f * r * r;
            gain[i] = (0.2f + 0.8f * energy[static_cast<size_t>(i)]) * b.alpha;
            inv[i] = 1.f / (b.fade * (0.85f + 0.4f * energy[static_cast<size_t>(i)]));
        }
        for (int y = 0; y < h; ++y) {
            const float v = static_cast<float>(y) / static_cast<float>(std::max(1, h - 1));
            // Kept off the top strip and the corner's text: fades in under the top, out above the lyric line.
            const float mask = smooth01(0.02f, 0.14f, v) * (1.f - smooth01(0.6f, 0.82f, v));
            // The three composited over each other, premultiplied (the back ribbon first).
            float alpha = 0, r = 0, g = 0, bl = 0;
            if (mask > 0.001f) {
                for (int i = 0; i < 3; ++i) {
                    const float d = yk[i] - v;   // > 0: above the lower edge
                    const float e = d >= 0 ? ex(d * inv[i]) : ex((d * 55.f) * (d * 55.f));
                    const float a = std::min(1.f, e * ray[i] * gain[i] * mask);
                    const Color& tc = tone[static_cast<size_t>(i)];
                    r = tc.r * a + (1.f - a) * r;
                    g = tc.g * a + (1.f - a) * g;
                    bl = tc.b * a + (1.f - a) * bl;
                    alpha = a + (1.f - a) * alpha;
                }
            }
            auto q = [](float f) { return static_cast<uint32_t>(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
            curtainTexels_[static_cast<size_t>(y) * w + x] = q(alpha) << 24 | q(r) << 16 | q(g) << 8 | q(bl);
        }
    }
    const uint64_t gen = gfx::Device::get().generation();
    const auto sz = curtains_ ? curtains_->GetPixelSize() : D2D1_SIZE_U{0, 0};
    if (!curtains_ || curtainGen_ != gen || static_cast<int>(sz.width) != w || static_cast<int>(sz.height) != h) {
        curtains_.Reset();
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        c.dc()->CreateBitmap(D2D1::SizeU(static_cast<UINT32>(w), static_cast<UINT32>(h)), curtainTexels_.data(),
                             static_cast<UINT32>(w) * 4, &props, &curtains_);
        curtainGen_ = gen;
    } else {
        curtains_->CopyFromMemory(nullptr, curtainTexels_.data(), static_cast<UINT32>(w) * 4);
    }
}

// Aurora: the curtains over the whole view (recomputed ~30 times a second while sound plays, linearly stretched) and
// a few motes drifting up, quicker and brighter with the treble.
void Visualizer::paintAurora(Canvas& c, const Rect& area, float alpha) {
    const Rect r = rect();
    const auto& L = analyzer_.levels();
    const int tw = std::clamp(static_cast<int>(std::ceil(r.w / 12.f)) + 1, 16, 360);
    const int th = std::clamp(static_cast<int>(std::ceil(r.h / 12.f)) + 1, 12, 240);
    const double now = ui::frame::now();
    const bool moving = motion_ && !analyzer_.resting();
    if (!curtains_ || curtainW_ != tw || curtainH_ != th || curtainGen_ != gfx::Device::get().generation() ||
        (moving && now - curtainAt_ >= kCurtainStepMs - 2)) {
        renderCurtains(c, tw, th);
        curtainAt_ = now;
    }
    if (curtains_) {
        const auto sz = curtains_->GetSize();
        const D2D1_RECT_F src{0.5f, 0.5f, sz.width - 0.5f, sz.height - 0.5f};
        const float lift = motion_ ? 0.88f + 0.12f * L.beat : 0.9f;
        c.dc()->DrawBitmap(curtains_.Get(), r.d2d(), alpha * lift, D2D1_INTERPOLATION_MODE_LINEAR, &src);
    }
    // Motes.
    const bool light = gfx::isLightTheme();
    const Color mc = light ? accent().base : colors().fgPrimary;
    const float T = static_cast<float>(drift_);
    for (auto& m : motes_) {
        // Position from the drift clock alone (no state per frame): rising, wrapping, a slow sideways sway.
        const float travel = m.speed * T * (1.f + 1.5f * (motion_ ? L.treble : 0.f));
        float y = m.y - std::fmod(travel, 0.75f);
        if (y < 0.08f) y += 0.75f;
        const float x = m.x + 0.012f * std::sin(T * 0.4f + m.phase);
        const float fade = smooth01(0.08f, 0.2f, y) * (1.f - smooth01(0.66f, 0.82f, y));
        const float twinkle = 0.55f + 0.45f * std::sin(T * (1.3f + m.speed * 40.f) + m.phase);
        const float a = (0.18f + 0.3f * twinkle) * fade * (1.f + (motion_ ? 0.6f * L.treble : 0.f)) * alpha * (light ? 0.55f : 1.f);
        if (a > 0.01f) c.fillCircle({area.x + x * area.w, r.h * y}, m.size, mc.withAlpha(std::min(0.8f, a)));
    }
}

void Visualizer::paintCorner(Canvas& c, const Rect& r) {
    const auto& col = colors();
    const auto* p = ctx().player;
    c.text(title_, {r.x, r.y, r.w, 24}, col.fgPrimary, gfx::VAlign::Center);
    c.text(artist_, {r.x, r.y + 26, r.w, 18}, col.fgSecondary, gfx::VAlign::Center);
    const float y = r.y + 56;
    if (p && p->isLive()) {
        drawLiveBadge(c, {r.x, y}, p->isPlaying());
        return;
    }
    const int64_t pos = p ? p->positionMs() : 0, dur = p ? p->durationMs() : 0;
    time_.setText(ui::formatDuration(pos) + L" / " + ui::formatDuration(dur));
    const float tw = std::ceil(time_.measure().w) + 1;
    const float lw = std::max(80.f, std::min(r.w - tw - 14, 300.f));
    const float frac = dur > 0 ? std::clamp(static_cast<float>(pos) / static_cast<float>(dur), 0.f, 1.f) : 0.f;
    c.fillRect({r.x, y - 1, lw, 2}, col.hairStrong);
    c.fillRect({r.x, y - 1, std::round(lw * frac), 2}, accent().base);
    c.text(time_, {r.x + lw + 12, y - 9, tw, 18}, backdrop_.labelColor(), gfx::VAlign::Center);
    if (p && !p->isPlaying() && p->current()) {
        const std::wstring paused = toUpperTr(tr(L"Duraklatıldı"));
        c.text(paused, type::monoLabel, {r.x + lw + 12 + tw + 12, y - 8, 160, 16}, backdrop_.labelColor(), gfx::TextAlign::Leading,
               gfx::VAlign::Center);
    }
}

void Visualizer::paintTop(Canvas& c, const Rect& r) {
    const float pad = std::round(std::clamp(r.w * 0.045f, 32.f, 72.f));
    label_.setText(toUpperTr(tr(L"Görselleştirici")) + L" · " + toUpperTr(modeName(mode_)));
    float left = r.w;
    for (auto* b : modeBtns_) left = std::min(left, b->rect().x);
    const float lw = std::max(0.f, left - 24 - pad);
    if (lw > 60) c.text(label_, {pad, 24, lw, 16}, backdrop_.labelColor(), gfx::VAlign::Center);
}

void Visualizer::paint(Canvas& c) {
    const auto t0 = std::chrono::steady_clock::now();
    const Rect r = rect();
    const float t = open_ * (closing_ ? std::clamp(exit_.value(), 0.f, 1.f) : 1.f);
    if (t <= 0.001f) return;
    if (t < 0.999f) c.pushOpacity(t);
    auto* p = ctx().player;
    const auto* track = p ? p->current() : nullptr;
    bool live = false;
    analyse(live);
    const double drift = backdrop_.paint(c, r, track, live);
    c.pushClip(r);
    const float mix = modeMix_;
    if (mix < 0.999f) paintMode(c, from_, contentRect_, 1.f - mix);
    paintMode(c, mode_, contentRect_, mix);
    c.popClip();
    paintLyric(c, lyricRect_);
    paintCorner(c, cornerRect_);

    // Controls fade out while the mouse rests (not while one of them has the keyboard focus ring), the pointer with
    // them.
    const auto* w = window();
    const bool keyboard = w && w->focusVisible() && w->focusedWidget() && w->focusedWidget()->parent() == this;
    bool overTop = false;   // the top strip is a caption area: no mouse moves reach us there
    if (POINT pt; w && !w->isFullscreen() && GetCursorPos(&pt) && ScreenToClient(w->hwnd(), &pt))
        overTop = pt.y >= 0 && pt.y < kTopH * w->scale() && pt.x >= 0 && pt.x < r.w * w->scale();
    if (ui::frame::realNow() - lastMove_ > kControlsIdleMs && !keyboard && !overTop) {
        if (controls_.target() > 0) controls_.to(0, ui::motion::slow);
    } else if (controls_.target() < 1) {
        controls_.to(1, ui::motion::fast);
    }
    if (const float a = controls_; a > 0.001f) {
        if (a < 0.999f) c.pushOpacity(a);
        paintTop(c, r);
        paintChildren(c);
        if (a < 0.999f) c.popLayer();
    }
    if (t < 0.999f) c.popLayer();

    // Frames while sound plays and until a pause has settled: every display refresh up to ~125 a second, every second
    // (third) one on faster displays (240 Hz: 120 a second; 144 / 165 Hz: all of them). The audio itself changes every
    // 10 ms (the device period), so frames beyond that only interpolate the smoothing, at full GPU cost. Reduce motion:
    // 10 a second. Then nothing at all; the backdrop's own drift steps while only it moves.
    if (auto* win = window()) {
        const bool moving = live || !analyzer_.resting() || !pulses_.empty() || lyricIn_.running();
        if (moving) {
            const double period = win->refreshPeriod();
            const int every = period > 0 ? std::max(1, static_cast<int>(std::lround(kFrameMs / period))) : 1;
            if (!motion_) win->invalidateAfter(kReducedFrameMs);
            else if (every == 1) ui::frame::requestNext();
            else ui::frame::requestNextEvery(every);   // presented with that sync interval: evenly paced
        } else if (drift > 0) {
            win->invalidateAfter(drift);
        }
    }
    // Dev: consecutive frames as PNGs (SHADETUBE_VIZ_SHOTS).
    if (auto& s = shots(); !s.folder.empty() && s.taken < s.count && !closing_) {
        static const double opened = ui::frame::realNow();
        if (ui::frame::realNow() - opened >= s.startMs + s.taken * s.gapMs) {
            const std::wstring file = (std::filesystem::path(s.folder) / (L"viz-" + std::to_wstring(s.taken) + L".png")).wstring();
            ++s.taken;
            Dispatcher::post([file] {
                if (auto* mw = ctx().window) mw->renderToPng(file);
            });
        }
        ui::frame::requestNext();
    }
    logStats(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void Visualizer::logStats(double paintMs) {
    static const bool enabled = devFlag(L"SHADETUBE_VIZ_LOG");
    if (!enabled) return;
    ++frames_;
    paintMsSum_ += paintMs;
    paintMsMax_ = std::max(paintMsMax_, paintMs);
    const double now = ui::frame::realNow();
    if (now - statsAt_ < 5000) return;
    const double secs = (now - statsAt_) / 1000.0;
    const auto bs = backdrop_.takeStats();
    const auto* w = window();
    ST_LOG_INFO("visualizer", "{} {:.0f}x{:.0f} DIP @{:.2f}{}: {:.1f} frames/s, paint {:.3f} ms avg / {:.2f} max, {:.1f} analyses/s, "
                "backdrop {:.1f} renders/s",
                modeId(mode_), rect().w, rect().h, w ? w->scale() : 1.f, w && w->isFullscreen() ? " full screen" : "", frames_ / secs,
                frames_ ? paintMsSum_ / frames_ : 0.0, paintMsMax_, (analyzer_.analyses() - analysesAt_) / secs, bs.renders / secs);
    statsAt_ = now;
    frames_ = 0;
    paintMsSum_ = paintMsMax_ = 0;
    analysesAt_ = analyzer_.analyses();
}

// ---- Input ----------------------------------------------------------------------------------------------------------

bool Visualizer::onKeyDown(const ui::KeyEvent& e) {
    showControls();
    if (e.ctrl || e.alt) return false;
    switch (e.vk) {
    case VK_ESCAPE: close(); return true;
    case 'F':
    case VK_F11: {
        auto* w = window();
        setFullscreen(!(w && w->isFullscreen()));
        return true;
    }
    case 'L': setLyricsShown(!Settings::get().visualizerLyrics); return true;
    case 'M': setMode(static_cast<Mode>((static_cast<int>(mode_) + (e.shift ? kModes - 1 : 1)) % kModes)); return true;
    default:
        if (e.vk >= '1' && e.vk < '1' + kModes && !e.shift) {
            setMode(static_cast<Mode>(e.vk - '1'));
            return true;
        }
        return false;   // Space, arrows, V...: the app's shortcuts
    }
}

bool Visualizer::onMouseDown(const ui::MouseEvent& e) {
    showControls();
    if (e.button == ui::MouseButton::Left && e.clicks == 2) {
        auto* w = window();
        setFullscreen(!(w && w->isFullscreen()));
    }
    return true;   // modal: swallow
}

void Visualizer::onMouseMove(const ui::MouseEvent&) { showControls(); }

LPCWSTR Visualizer::cursor() const { return controls_.target() <= 0 && controls_.value() < 0.05f ? nullptr : IDC_ARROW; }

// ---- Feature ----------------------------------------------------------------------------------------------------------

void initVisualizer() {
    commands::setHandler("visualizer", [] {
        Visualizer::toggle();
        return true;
    });
    ctx().trackChangedHooks.push_back([](const catalog::Track&) { Visualizer::trackChanged(); });
    // Play / pause after a pause has settled: nothing asks for frames, so the change does (and a station's title).
    ctx().playerChangedHooks.push_back([] {
        if (g_view) {
            g_view->invalidate();
            Visualizer::trackChanged();
        }
    });
    // One full-window view at a time: the full-screen lyrics (key, palette, Now Playing) close this one first.
    if (auto lyricsToggle = ctx().toggleLyricsFullscreen) {
        ctx().toggleLyricsFullscreen = [lyricsToggle] {
            if (g_view) g_view->close();
            lyricsToggle();
        };
    }
}

} // namespace st::app
