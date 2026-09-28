#include "app/LyricsFullscreen.h"

#include "app/AppContext.h"
#include "app/InternetRadio.h"
#include "app/LyricsService.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "player/Player.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

namespace {

LyricsFullscreen* g_view = nullptr;

constexpr float kTopH = 64, kFooterH = 96;
constexpr double kControlsIdleMs = 2500;

// Hit-test boxes of the text range [pos, pos + len) of `layout`, one per visual line (layout coordinates).
std::vector<DWRITE_HIT_TEST_METRICS> rangeBoxes(IDWriteTextLayout* layout, UINT32 pos, UINT32 len) {
    std::vector<DWRITE_HIT_TEST_METRICS> m;
    if (!layout || len == 0) return m;
    UINT32 n = 0;
    layout->HitTestTextRange(pos, len, 0, 0, nullptr, 0, &n);
    if (n == 0) return m;
    m.resize(n);
    if (FAILED(layout->HitTestTextRange(pos, len, 0, 0, m.data(), n, &n))) n = 0;
    m.resize(n);
    return m;
}

float widthOf(const std::vector<DWRITE_HIT_TEST_METRICS>& boxes) {
    float w = 0;
    for (const auto& b : boxes) w += b.width;
    return w;
}

} // namespace

// ---- Opening / closing -----------------------------------------------------------------------------------------

void LyricsFullscreen::toggle() {
    if (g_view) {
        g_view->close();
        return;
    }
    auto* w = ctx().window;
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!w || !t) return;
    if (radio::isStationId(t->id)) {
        toast(tr(L"Canlı radyo yayınlarında şarkı sözü gösterilmez"));
        return;
    }
    w->pushOverlay(std::make_unique<LyricsFullscreen>(), true);
}

bool LyricsFullscreen::isOpen() { return g_view != nullptr; }

void LyricsFullscreen::trackChanged() {
    if (g_view) g_view->load();
}

LyricsFullscreen::LyricsFullscreen()
    : title_({}, type::sectionTitle), artist_({}, type::secondary), time_({}, type::monoDuration), label_({}, type::monoLabel) {
    g_view = this;
    drag_ = add<ui::Widget>();
    drag_->isDragRegion = true;
    earlier_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "minus");
    earlier_->setTooltip(tr(L"Sözleri daha erken göster"));
    earlier_->onClick = [this] { shiftOffset(-kLyricsOffsetStepMs); };
    offset_ = add<ui::Button>(ui::ButtonKind::Ghost, formatLyricsOffset(0));
    offset_->setTooltip(tr(L"Söz zamanlamasını sıfırla"));
    offset_->onClick = [this] {
        if (!trackId_.empty()) setLyricsOffset(trackId_, 0);
        syncControls();
    };
    later_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "plus");
    later_->setTooltip(tr(L"Sözleri daha geç göster"));
    later_->onClick = [this] { shiftOffset(kLyricsOffsetStepMs); };
    close_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "close");
    close_->setTooltip(tr(L"Kapat (Esc)"));
    close_->onClick = [this] { close(); };
    open_.to(1, ui::motion::medium, ui::Ease::Decelerate);
    controls_.snap(1);
    lastMove_ = ui::frame::realNow();
    load();
}

LyricsFullscreen::~LyricsFullscreen() {
    if (g_view == this) g_view = nullptr;
}

void LyricsFullscreen::close() {
    if (g_view == this) g_view = nullptr;
    life_.renew();
    if (auto* w = window()) w->removeOverlay(this);
}

// ---- Lyrics ----------------------------------------------------------------------------------------------------

void LyricsFullscreen::load() {
    life_.renew();
    lyrics_ = {};
    lines_.clear();
    lineH_.clear();
    lineRects_.clear();
    active_ = -1;
    hoverLine_ = -1;
    manualUntil_ = 0;
    scroll_.snap(0);
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    trackId_ = t ? t->id : std::string();
    title_.setText(t ? toWide(t->name) : std::wstring());
    artist_.setText(t ? toWide(t->artistLine()) : std::wstring());
    syncControls();
    invalidate();
    if (!t) {
        state_ = State::None;
        return;
    }
    if (radio::isStationId(t->id)) {
        state_ = State::Live;
        return;
    }
    if (!Settings::get().lyricsEnabled) {
        state_ = State::Missing;
        return;
    }
    state_ = State::Loading;
    const lyrics::Query q = lyricsQueryFor(*t);
    async(Priority::High, life_.ref(), [q] { return lyrics::fetch(q); },
          [this](Result<std::optional<lyrics::Lyrics>> r) {
              if (!r || !*r) {
                  state_ = State::Missing;
              } else {
                  lyrics_ = std::move(**r);
                  if (lyrics_.instrumental) state_ = State::Instrumental;
                  else if (lyrics_.lines.empty()) state_ = State::Missing;
                  else state_ = lyrics_.synced ? State::Synced : State::Plain;
              }
              gfx::TextOptions wrap;
              wrap.wrap = true;
              wrap.align = gfx::TextAlign::Center;
              lines_.clear();
              lines_.reserve(lyrics_.lines.size());
              for (const auto& l : lyrics_.lines) lines_.emplace_back(l.text.empty() ? L"♪" : toWide(l.text), style_, wrap);
              lineH_.assign(lines_.size(), -1.f);
              syncControls();
              requestLayout();
              invalidate();
          });
}

void LyricsFullscreen::syncControls() {
    const bool synced = state_ == State::Synced;
    earlier_->setVisible(synced);
    offset_->setVisible(synced);
    later_->setVisible(synced);
    offset_->setLabel(formatLyricsOffset(trackId_.empty() ? 0 : lyricsOffset(trackId_)));
    requestLayout();
}

void LyricsFullscreen::shiftOffset(int ms) {
    if (trackId_.empty() || state_ != State::Synced) return;
    setLyricsOffset(trackId_, lyricsOffset(trackId_) + ms);
    syncControls();
    showControls();
}

void LyricsFullscreen::showControls() {
    lastMove_ = ui::frame::realNow();
    if (controls_.target() < 1) controls_.to(1, ui::motion::fast);
    if (auto* w = window()) w->invalidateAfter(kControlsIdleMs + 50);
    invalidate();
}

// ---- Layout ----------------------------------------------------------------------------------------------------

void LyricsFullscreen::layout() {
    const Rect r = rect();
    // Type scales with the window: large enough to read across a room, quantized so resizing reuses layouts.
    const float size = std::clamp(std::round(r.h * 0.056f / 4.f) * 4.f, 28.f, 64.f);
    gfx::TextStyle style{size, 800, 1.18f, -0.03f};
    if (!(style == style_)) {
        style_ = style;
        for (auto& l : lines_) l.setStyle(style_);
        lineH_.assign(lines_.size(), -1.f);
    }
    const float pad = std::max(48.f, r.w * 0.08f);
    const float w = std::min(r.w - pad * 2, 1200.f);
    lyricsArea_ = {(r.w - w) * 0.5f, kTopH, w, std::max(100.f, r.h - kTopH - kFooterH)};

    const float cy = 32, bs = 32;
    float x = r.w - 24 - bs;
    close_->setRect({x, cy - bs / 2, bs, bs});
    x -= 16;
    if (later_->visible()) {
        x -= bs;
        later_->setRect({x, cy - bs / 2, bs, bs});
        const float ow = std::max(72.f, offset_->naturalWidth());
        x -= ow + 4;
        offset_->setRect({x, cy - 16, ow, 32});
        x -= bs + 4;
        earlier_->setRect({x, cy - bs / 2, bs, bs});
    }
    drag_->setRect({0, 0, std::max(0.f, x - 16), kTopH});   // the controls keep getting mouse moves
}

float LyricsFullscreen::lineHeight(int i, float width) {
    if (width != lineHW_ || lineH_.size() != lines_.size()) {
        lineH_.assign(lines_.size(), -1.f);
        lineHW_ = width;
    }
    if (lineH_[i] < 0) {
        lineH_[i] = lines_[i].measure(width).h;
        lines_[i].reset();
    }
    return lineH_[i];
}

int LyricsFullscreen::lineAt(gfx::Point p) const {
    for (int i = 0; i < static_cast<int>(lineRects_.size()); ++i)
        if (lineRects_[i].contains(p) && lyricsArea_.contains(p)) return i;
    return -1;
}

// ---- Painting --------------------------------------------------------------------------------------------------

// The artwork blurred over the whole window, like Now Playing's backdrop (quarter resolution; rebuilt when the art,
// size, device or theme changes).
void LyricsFullscreen::paintBackdrop(Canvas& c, const Rect& r) {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    const auto& col = colors();
    ID2D1Bitmap1* art = nullptr;
    std::string url;
    if (t && !radio::isStationId(t->id)) {
        if (const auto* img = catalog::pickImage(t->album.images, 300)) {
            url = img->url;
            art = gfx::ImageCache::get().request(url, 300);
        }
    }
    const uint64_t gen = gfx::Device::get().generation();
    const uint64_t themeGen = gfx::Theme::get().generation();
    if (art && (url != backdropUrl_ || gen != backdropGen_ || themeGen != backdropTheme_ || r.w != backdropW_ ||
                r.h != backdropH_)) {
        auto* dc = c.dc();
        const UINT bw = std::max(1u, static_cast<UINT>(r.w / 4)), bh = std::max(1u, static_cast<UINT>(r.h / 4));
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> bmp;
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(bw, bh), nullptr, 0, &props, &bmp))) {
            Microsoft::WRL::ComPtr<ID2D1Image> old;
            dc->GetTarget(&old);
            D2D1_MATRIX_3X2_F oldT;
            dc->GetTransform(&oldT);
            float dx, dy;
            dc->GetDpi(&dx, &dy);
            dc->SetTarget(bmp.Get());
            dc->SetDpi(96, 96);
            dc->SetTransform(D2D1::Matrix3x2F::Identity());
            dc->Clear(col.bgBase.d2d());
            {
                Canvas off(dc, 1.f);
                off.blurredImage(art, {0, 0, static_cast<float>(bw), static_cast<float>(bh)}, 48.f / 4.f * 3.f, 1.2f, 1.1f);
            }
            dc->SetTarget(old.Get());
            dc->SetDpi(dx, dy);
            dc->SetTransform(oldT);
            const bool newArt = !backdrop_ || url != backdropUrl_;
            backdrop_ = bmp;
            backdropUrl_ = url;
            backdropGen_ = gen;
            backdropTheme_ = themeGen;
            backdropW_ = r.w;
            backdropH_ = r.h;
            if (newArt) {
                backdropFade_.snap(0);
                backdropFade_.to(1, 600);
            }
        }
    }
    c.fillRect(r, col.bgBase);
    if (backdrop_) c.dc()->DrawBitmap(backdrop_.Get(), r.d2d(), backdropFade_, D2D1_INTERPOLATION_MODE_LINEAR);
    // A heavier scrim than Now Playing's: the lyrics are the only thing on screen and must read from across a room.
    c.fillRect(r, col.bgBase.withAlpha(gfx::isLightTheme() ? 0.78f : 0.62f));
    c.fillRadialGradient(r, {r.cx(), r.cy()}, std::max(r.w, r.h) * 0.6f, accent().base.withAlpha(0.08f), accent().base.withAlpha(0.f));
}

// The sung part of the active line in the accent: per word when the lyrics carry word times, else a sweep across the
// line lasting until the next line (capped by the line's length, so a long instrumental gap doesn't crawl).
void LyricsFullscreen::paintSweep(Canvas& c, int i, const Rect& lr, int posMs) {
    const auto& line = lyrics_.lines[i];
    auto& text = lines_[i];
    auto* layout = text.layout(lr.w);
    const UINT32 len = static_cast<UINT32>(text.text().size());
    const auto boxes = rangeBoxes(layout, 0, len);
    const float total = widthOf(boxes);
    if (total <= 0 || line.text.empty()) return;
    const int next = i + 1 < static_cast<int>(lyrics_.lines.size()) ? lyrics_.lines[i + 1].timeMs : line.timeMs + 8000;
    float sweep = 0;
    if (!line.words.empty()) {
        // Word k is being sung: the fill runs through its characters until the next word starts.
        int k = -1;
        for (int w = 0; w < static_cast<int>(line.words.size()); ++w)
            if (line.words[w].timeMs <= posMs) k = w;
        if (k >= 0) {
            UINT32 start = 0;
            for (int w = 0; w < k; ++w) start += static_cast<UINT32>(toWide(line.words[w].text).size());
            const UINT32 wlen = static_cast<UINT32>(toWide(line.words[k].text).size());
            const int t0 = line.words[k].timeMs;
            const int t1 = k + 1 < static_cast<int>(line.words.size())
                               ? line.words[k + 1].timeMs
                               : std::min(next, t0 + std::max(400, static_cast<int>(wlen) * 110));
            const float frac = t1 > t0 ? std::clamp(static_cast<float>(posMs - t0) / static_cast<float>(t1 - t0), 0.f, 1.f) : 1.f;
            const float chars = std::min(static_cast<float>(len), static_cast<float>(start) + frac * static_cast<float>(wlen));
            const UINT32 whole = static_cast<UINT32>(chars);
            sweep = widthOf(rangeBoxes(layout, 0, whole));
            if (whole < len) sweep += (chars - static_cast<float>(whole)) * widthOf(rangeBoxes(layout, whole, 1));
        }
    } else {
        const int dur = std::max(250, std::min(next - line.timeMs - 150, std::max(1500, static_cast<int>(len) * 80)));
        sweep = total * std::clamp(static_cast<float>(posMs - line.timeMs) / static_cast<float>(dur), 0.f, 1.f);
    }
    float acc = 0;
    for (const auto& b : boxes) {
        const float cover = std::clamp(sweep - acc, 0.f, b.width);
        if (cover > 0) {
            c.pushClip({lr.x + b.left, lr.y + b.top - 8, cover, b.height + 16});
            c.text(text, lr, accent().base);
            c.popClip();
        }
        acc += b.width;
    }
}

void LyricsFullscreen::paintLyrics(Canvas& c, const Rect& area) {
    const auto& col = colors();
    auto centered = [&](const std::wstring& big, const std::wstring& note) {
        c.text(big, type::headline, {area.x, area.cy() - 40, area.w, 34}, col.fgSecondary, gfx::TextAlign::Center);
        if (!note.empty())
            c.text(note, type::monoLabel, {area.x, area.cy() + 6, area.w, 16}, col.fgTertiary, gfx::TextAlign::Center);
    };
    switch (state_) {
    case State::None: return;
    case State::Loading:
        for (int i = 0; i < 5; ++i) {
            const float w = area.w * (0.35f + 0.1f * ((i * 7) % 4));
            c.skeleton({area.cx() - w / 2, area.cy() - 110 + i * 52.f, w, 28}, 2, ui::frame::now());
        }
        ui::frame::requestNext();
        return;
    case State::Missing:
        centered(tr(L"Bu şarkı için söz bulunamadı."),
                 Settings::get().lyricsEnabled ? std::wstring() : toUpperTr(tr(L"Şarkı sözleri Ayarlar'da kapalı")));
        return;
    case State::Instrumental: centered(tr(L"♪  Enstrümantal"), tr(L"SÖZ YOK · SADECE MÜZİK")); return;
    case State::Live: centered(tr(L"Canlı radyo yayınlarında şarkı sözü gösterilmez"), {}); return;
    default: break;
    }
    const auto* p = ctx().player;
    const bool synced = state_ == State::Synced;
    const int pos = lyricsClockMs(trackId_);   // on the song's timeline, offset applied
    active_ = synced ? lyrics::activeLine(lyrics_, pos + 150) : -1;     // 150 ms lookahead feels in sync

    const float gap = style_.size * 0.55f;
    lineRects_.resize(lines_.size());
    float y = 0, activeTop = 0, activeH = 0;
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        const float h = lineHeight(i, area.w);
        lineRects_[i] = {area.x, y, area.w, h};
        if (i == active_) {
            activeTop = y;
            activeH = h;
        }
        y += h + gap;
    }
    const float total = y;
    float target;
    if (ui::frame::realNow() < manualUntil_) target = manualOffset_;
    else if (synced && active_ >= 0) target = activeTop + activeH * 0.5f - area.h * 0.45f;
    else if (synced) target = -area.h * 0.35f;
    else target = scroll_.target();
    target = std::clamp(target, -area.h * 0.5f, std::max(0.f, total - area.h * 0.5f));
    if (std::abs(target - scroll_.target()) > 0.5f) scroll_.to(target, ui::motion::lyrics, ui::Ease::Standard);
    const float offY = scroll_.value();

    // The top and bottom fade out: lines are clipped to a band a little taller than the area.
    const Rect band{0, area.y - 24, rect().w, area.h + 48};
    c.pushClip(band);
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        Rect lr = lineRects_[i].offset(0, area.y - offY);
        lineRects_[i] = lr;
        if (lr.bottom() < band.y || lr.y > band.bottom()) {
            lines_[i].reset();   // scrolled away: drop its layout
            continue;
        }
        // Lines near the band's edges fade out.
        const float edge = std::min(lr.cy() - band.y, band.bottom() - lr.cy());
        const float fade = std::clamp(edge / 120.f, 0.f, 1.f);
        Color colr;
        if (!synced) colr = col.fgSecondary;
        else if (i == active_) colr = col.fgPrimary;
        else {
            const int dist = std::abs(i - active_);
            const float a = i < active_ ? 0.32f : 0.24f;
            colr = col.fgPrimary.withAlpha(dist >= 3 ? a * 0.6f : a);
        }
        if (i == hoverLine_ && synced && i != active_) colr = col.fgPrimary.withAlpha(0.7f);
        colr = colr.mulAlpha(fade);
        c.text(lines_[i], lr, colr);
        if (synced && i == active_ && !ui::motion::reduced() && fade > 0.99f) paintSweep(c, i, lr, pos + 150);
    }
    c.popClip();
    if (synced && p && p->isPlaying()) {
        if (auto* w = window()) w->invalidateAfter(ui::motion::reduced() ? 250 : 33);
    }
}

void LyricsFullscreen::paintFooter(Canvas& c, const Rect& r) {
    const auto& col = colors();
    const auto* p = ctx().player;
    const float pad = std::max(48.f, r.w * 0.08f);
    const float y = r.bottom() - kFooterH + 24;
    const bool live = p && p->isLive();
    const int64_t pos = p ? p->positionMs() : 0, dur = p ? p->durationMs() : 0;
    float tw = 0;
    if (!live && dur > 0) {
        time_.setText(ui::formatDuration(pos) + L" / " + ui::formatDuration(dur));
        tw = std::ceil(time_.measure().w) + 1;
        c.text(time_, {r.right() - pad - tw, y + 8, tw, 18}, col.fgTertiary, gfx::VAlign::Center);
    }
    const float iw = std::max(40.f, r.w - pad * 2 - tw - 24);
    c.text(title_, {pad, y, iw, 24}, col.fgPrimary, gfx::VAlign::Center);
    c.text(artist_, {pad, y + 26, iw, 18}, col.fgSecondary, gfx::VAlign::Center);
    // Thin progress bar along the bottom edge.
    if (!live && dur > 0) {
        const float frac = std::clamp(static_cast<float>(pos) / static_cast<float>(dur), 0.f, 1.f);
        c.fillRect({0, r.bottom() - 3, r.w, 3}, col.hairSubtle);
        c.fillRect({0, r.bottom() - 3, r.w * frac, 3}, accent().base);
    }
}

void LyricsFullscreen::paintTop(Canvas& c, const Rect& r) {
    const auto& col = colors();
    std::wstring label = toUpperTr(tr(L"Şarkı sözleri"));
    if ((state_ == State::Synced || state_ == State::Plain) && !lyrics_.source.empty())
        label += L" · " + lyricsSourceLabel(lyrics_.source);
    if (state_ == State::Plain) label += L" · " + toUpperTr(tr(L"Senkronize değil"));
    label_.setText(label);
    const float pad = std::max(48.f, r.w * 0.08f);
    c.text(label_, {pad, 24, std::max(40.f, r.w - pad - 260), 16}, col.fgTertiary, gfx::VAlign::Center);
    // The offset value flashes in the accent right after a change.
    if (offset_->visible()) {
        const double since = ui::frame::realNow() - lyricsOffsetChangedAt();
        if (lyricsOffsetChangedAt() > 0 && since < 1200) {
            const Rect o = offset_->rect();
            c.fillPill(o, accent().base.withAlpha(0.18f * static_cast<float>(1.0 - since / 1200)));
            ui::frame::requestNext();
        }
    }
}

void LyricsFullscreen::paint(Canvas& c) {
    const Rect r = rect();
    const float t = open_;
    if (t < 0.999f) c.pushOpacity(t);
    paintBackdrop(c, r);
    paintLyrics(c, lyricsArea_);
    paintFooter(c, r);
    // Controls fade out while the mouse rests (not while a control has the keyboard focus ring).
    const auto* w = window();
    const bool keyboard = w && w->focusVisible() && w->focusedWidget() && w->focusedWidget()->parent() == this;
    // The pointer resting on the top strip (a caption area: no mouse moves reach us there) keeps them too.
    bool overTop = false;
    if (POINT pt; w && GetCursorPos(&pt) && ScreenToClient(w->hwnd(), &pt))
        overTop = pt.y >= 0 && pt.y < kTopH * w->scale() && pt.x >= 0 && pt.x < r.w * w->scale();
    if (ui::frame::realNow() - lastMove_ > kControlsIdleMs && !keyboard && !overTop) {
        if (controls_.target() > 0) controls_.to(0, ui::motion::slow);
    } else if (controls_.target() < 1) {
        controls_.to(1, ui::motion::fast);
    }
    const float a = controls_;
    if (a > 0.001f) {
        if (a < 0.999f) c.pushOpacity(a);
        paintTop(c, r);
        paintChildren(c);
        if (a < 0.999f) c.popLayer();
    }
    if (t < 0.999f) c.popLayer();
}

// ---- Input -----------------------------------------------------------------------------------------------------

bool LyricsFullscreen::onKeyDown(const ui::KeyEvent& e) {
    showControls();
    if (e.ctrl || e.alt) return false;
    switch (e.vk) {
    case VK_ESCAPE:
    case 'F': close(); return true;
    case VK_OEM_MINUS:
    case VK_SUBTRACT: shiftOffset(-kLyricsOffsetStepMs); return true;
    case VK_OEM_PLUS:
    case VK_ADD: shiftOffset(kLyricsOffsetStepMs); return true;
    default: return false;   // Space, arrows...: the app's player shortcuts
    }
}

bool LyricsFullscreen::onMouseDown(const ui::MouseEvent& e) {
    showControls();
    if (state_ != State::Synced || e.button != ui::MouseButton::Left) return true;   // modal: swallow
    const int i = lineAt(e.pos);
    if (i >= 0 && lyrics_.lines[i].timeMs >= 0) {
        seekToLyricsTime(trackId_, lyrics_.lines[i].timeMs);
        manualUntil_ = 0;
    }
    return true;
}

void LyricsFullscreen::onMouseMove(const ui::MouseEvent& e) {
    showControls();
    const int i = state_ == State::Synced ? lineAt(e.pos) : -1;
    if (i != hoverLine_) {
        hoverLine_ = i;
        invalidate();
    }
}

void LyricsFullscreen::onMouseLeave() {
    if (hoverLine_ >= 0) {
        hoverLine_ = -1;
        invalidate();
    }
}

bool LyricsFullscreen::onWheel(float dy, float, const ui::MouseEvent&) {
    if (lines_.empty()) return true;
    manualOffset_ = (ui::frame::realNow() < manualUntil_ ? manualOffset_ : scroll_.value()) - dy * 110.f;
    manualUntil_ = ui::frame::realNow() + 3000;   // snaps back to the active line after 3 s
    if (auto* w = window()) w->invalidateAfter(3050);
    invalidate();
    return true;
}

} // namespace st::app
