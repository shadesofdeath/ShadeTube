#include "app/NowPlaying.h"

#include "app/Shell.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <cmath>
#include <numbers>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

NowPlayingView::NowPlayingView()
    : title_({}, type::displayS), subtitle_({}, type::bodyL.withSize(17)), meta_({}, type::monoMeta) {
    queue_ = add<QueuePanel>();
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.maxLines = 2;
    title_.setOptions(wrap);
}

void NowPlayingView::activate() {
    onTrackChanged();
    queue_->sync();
    openedAt_ = ui::frame::realNow();
    trimmed_ = false;
}

void NowPlayingView::deactivate() {
    backdrop_.Reset();
    backdropUrl_.clear();
    shadow_.Reset();
    lines_.clear();
    lines_.shrink_to_fit();
    lineH_.clear();
    lineH_.shrink_to_fit();
    lineRects_.clear();
    lineRects_.shrink_to_fit();
    lyrics_ = {};
    activeText_ = {};
    active_ = -2;
    // The display-size title / subtitle / meta layouts are rebuilt on the next open.
    title_.reset();
    subtitle_.reset();
    meta_.reset();
    trackId_.clear();
    state_ = LyricsState::None;
    life_.renew();
}

void NowPlayingView::onTrackChanged() {
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (!t) return;
    title_.setText(toWide(t->name));
    std::wstring sub = toWide(t->artistLine());
    if (!t->album.name.empty()) sub += L" · " + toWide(t->album.name);
    subtitle_.setText(sub);
    if (t->id != trackId_ && visible()) {
        trackId_ = t->id;
        fetchLyrics();
    }
    queue_->sync();
    invalidate();
}

void NowPlayingView::fetchLyrics() {
    life_.renew();
    lines_.clear();
    lineH_.clear();
    lineRects_.clear();
    active_ = -2;
    scroll_.snap(0);
    manualUntil_ = 0;
    const auto* t = ctx().player->current();
    if (!t || !Settings::get().lyricsEnabled) {
        state_ = LyricsState::Missing;
        return;
    }
    state_ = LyricsState::Loading;
    lyrics::Query q;
    q.cacheKey = t->id.empty() ? t->name : t->id;
    q.title = t->name;
    q.artist = t->artists.empty() ? "" : t->artists[0].name;
    q.album = t->album.name;
    q.durationSec = t->durationMs / 1000;
    async(Priority::High, life_.ref(), [q] { return lyrics::fetch(q); },
          [this](Result<std::optional<lyrics::Lyrics>> r) {
              if (!r || !*r) {
                  state_ = LyricsState::Missing;
                  invalidate();
                  return;
              }
              lyrics_ = std::move(**r);
              if (lyrics_.instrumental) state_ = LyricsState::Instrumental;
              else if (lyrics_.lines.empty()) state_ = LyricsState::Missing;
              else state_ = lyrics_.synced ? LyricsState::Synced : LyricsState::Plain;
              gfx::TextOptions wrap;
              wrap.wrap = true;
              lines_.clear();
              lines_.reserve(lyrics_.lines.size());
              for (const auto& l : lyrics_.lines) {
                  lines_.emplace_back(l.text.empty() ? L"♪" : toWide(l.text), type::lyricInactive, wrap);
              }
              lineH_.assign(lines_.size(), -1.f);
              activeText_ = gfx::Text({}, type::lyricActive, wrap);
              invalidate();
          });
}

void NowPlayingView::layout() {
    const Rect r = rect();
    queue_->setRect({r.w - gfx::metrics::queueW, 0, gfx::metrics::queueW, r.h});
    const float artSize = std::clamp(std::min(r.h - 220, (r.w - gfx::metrics::queueW) * 0.36f), 200.f, 488.f);
    artRect_ = {64, std::max(40.f, (r.h - artSize - 140) * 0.5f), artSize, artSize};
    const float lx = artRect_.right() + 64;
    lyricsRect_ = {lx, 0, std::max(200.f, r.w - gfx::metrics::queueW - 48 - lx), r.h};
}

void NowPlayingView::paintBackdrop(Canvas& c, const Rect& r) {
    const auto* t = ctx().player->current();
    const auto& col = colors();
    ID2D1Bitmap1* art = nullptr;
    std::string url;
    if (t) {
        if (const auto* img = catalog::pickImage(t->album.images, 300)) {
            url = img->url;
            art = gfx::ImageCache::get().request(url, 300);
        }
    }
    // Rebuild the quarter-resolution blurred backdrop when the art/size/device/theme changes (it bakes bg.base).
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
                // Canvas over the offscreen target: backdrop = art -> scale 1.15 -> blur 40 -> saturate 1.1.
                Canvas off(dc, 1.f);
                off.blurredImage(art, {0, 0, static_cast<float>(bw), static_cast<float>(bh)}, 40.f / 4.f * 3.f, 1.15f, 1.1f);
            }
            dc->SetTarget(old.Get());
            dc->SetDpi(dx, dy);
            dc->SetTransform(oldT);
            backdrop_ = bmp;
            backdropUrl_ = url;
            backdropGen_ = gen;
            backdropTheme_ = themeGen;
            backdropW_ = r.w;
            backdropH_ = r.h;
            backdropFade_.snap(0);
            backdropFade_.to(1, 600);
            // The blur's cached intermediates are released by the settle trim in paint(), not here: clearing D2D's
            // caches while the view is still building up only made private bytes grow (measured).
        }
    }
    c.fillRect(r, col.bgBase);
    if (backdrop_) c.dc()->DrawBitmap(backdrop_.Get(), r.d2d(), backdropFade_, D2D1_INTERPOLATION_MODE_LINEAR);
    // Scrim (effects.backdropBlur: bg.base at 55 %). On paper the blurred art needs more of it for fg.primary lyrics
    // to keep their contrast over dark covers.
    c.fillRect(r, col.bgBase.withAlpha(gfx::isLightTheme() ? 0.72f : 0.55f));
    // Accent glow behind the art (nowPlayingGlow) + a soft floor gradient for the player bar.
    c.fillRadialGradient(r, {artRect_.cx(), artRect_.cy()}, artRect_.w * 0.95f, accent().base.withAlpha(0.14f),
                         accent().base.withAlpha(0.f));
    c.fillLinearGradient({r.x, r.bottom() - 160, r.w, 160}, {0, r.bottom() - 160}, {0, r.bottom()}, col.bgBase.withAlpha(0),
                         col.bgBase.withAlpha(0.7f));
}

// nowPlayingArtShadow (blur 60, offset y 30, shadow.art) without the Shadow effect. The Gaussian blur of the art's
// rectangle is separable — alpha(x, y) = gx(x) * gy(y), each an erf step — so it is computed once on the CPU into a
// small bitmap (1 px per 4 DIP: after a 20 DIP std-dev blur only low frequencies are left) and stretched with linear
// filtering. The effect re-ran every frame here (the view repaints at 20-30 fps): a command list, a blur pass and its
// intermediate textures per frame, for a result that only changes with the art size / theme. The bitmap is ~100 KB.
void NowPlayingView::paintArtShadow(Canvas& c) {
    constexpr float kSigma = 60.f / 3.f, kMargin = 3 * kSigma, kOffsetY = 30.f, kDipPerPx = 4.f;
    const Color color = colors().shadowArt;
    const Rect dst{artRect_.x - kMargin, artRect_.y + kOffsetY - kMargin, artRect_.w + 2 * kMargin, artRect_.h + 2 * kMargin};
    const uint64_t gen = gfx::Device::get().generation(), themeGen = gfx::Theme::get().generation();
    if (!shadow_ || shadowArt_ != artRect_.w || shadowGen_ != gen || shadowTheme_ != themeGen) {
        shadow_.Reset();
        const UINT w = static_cast<UINT>(std::ceil(dst.w / kDipPerPx)), h = static_cast<UINT>(std::ceil(dst.h / kDipPerPx));
        // Coverage of [lo, hi] blurred, sampled at pixel centers (DIP offsets from dst's origin).
        auto profile = [](UINT n, float len, float lo, float hi) {
            std::vector<float> v(n);
            const float k = 1.f / (kSigma * std::numbers::sqrt2_v<float>);
            for (UINT i = 0; i < n; ++i) {
                const float x = (static_cast<float>(i) + 0.5f) * len / static_cast<float>(n);
                v[i] = 0.5f * (std::erf((x - lo) * k) - std::erf((x - hi) * k));
            }
            return v;
        };
        const auto gx = profile(w, dst.w, kMargin, kMargin + artRect_.w);
        const auto gy = profile(h, dst.h, kMargin, kMargin + artRect_.h);
        std::vector<uint32_t> px(static_cast<size_t>(w) * h);
        auto channel = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
        for (UINT y = 0; y < h; ++y) {
            for (UINT x = 0; x < w; ++x) {
                const float a = color.a * gx[x] * gy[y];   // premultiplied BGRA, like the effect's output
                px[static_cast<size_t>(y) * w + x] =
                    channel(color.b * a) | channel(color.g * a) << 8 | channel(color.r * a) << 16 | channel(a) << 24;
            }
        }
        const auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE,
                                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(c.dc()->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &props, &shadow_))) return;
        shadowArt_ = artRect_.w;
        shadowGen_ = gen;
        shadowTheme_ = themeGen;
    }
    c.dc()->DrawBitmap(shadow_.Get(), dst.d2d(), 1.f, D2D1_INTERPOLATION_MODE_LINEAR);
}

// Heights are cached per line; a line's layout is dropped right after measuring and rebuilt only while the line is on
// screen (a long song's wrapped 28 px layouts add up).
float NowPlayingView::lineHeight(int i, float width) {
    if (i == active_) {
        activeText_.setText(lines_[i].text());
        return activeText_.measure(width).h;
    }
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

void NowPlayingView::paintLyrics(Canvas& c, const Rect& r) {
    const auto& col = colors();
    const auto& acc = accent();
    c.text(state_ == LyricsState::Synced ? tr(L"ŞARKI SÖZLERİ · SENKRONİZE") : toUpperTr(tr(L"Şarkı sözleri")), type::monoLabel,
           {r.x, r.y + 48, r.w, 16}, col.fgTertiary);
    const Rect area{r.x, r.y + 96, r.w, r.h - 96 - 24};
    auto centered = [&](const wchar_t* big, const wchar_t* note) {
        c.text(big, type::headline, {area.x, area.cy() - 40, area.w, 34}, col.fgSecondary);
        c.text(note, type::monoLabel, {area.x, area.cy() + 4, area.w, 16}, col.fgTertiary);
    };
    switch (state_) {
    case LyricsState::Loading:
        for (int i = 0; i < 5; ++i)
            c.skeleton({area.x, area.cy() - 100 + i * 48.f, area.w * (0.4f + 0.1f * ((i * 7) % 4)), 22}, 2, ui::frame::now());
        ui::frame::requestNext();
        return;
    case LyricsState::Missing: centered(tr(L"Bu şarkı için söz bulunamadı."), tr(L"KAYNAK · LRCLIB")); return;
    case LyricsState::Instrumental: centered(tr(L"♪  Enstrümantal"), tr(L"SÖZ YOK · SADECE MÜZİK")); return;
    case LyricsState::None: return;
    default: break;
    }
    const auto* p = ctx().player;
    const int pos = static_cast<int>(p->positionMs());
    const bool synced = state_ == LyricsState::Synced;
    const int active = synced ? lyrics::activeLine(lyrics_, pos + 150) : -1;   // 150 ms lookahead feels in sync
    if (active != active_) {
        active_ = active;
        activeText_.setText(active >= 0 ? lines_[active].text() : L"");
    }
    // Layout all lines (heights cached inside Text objects) and find the active line's top.
    const float gap = 18;
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
    else if (synced) target = -area.h * 0.3f;
    else target = scroll_.target();
    target = std::clamp(target, -area.h * 0.5f, std::max(0.f, total - area.h * 0.5f));
    if (std::abs(target - scroll_.target()) > 0.5f) scroll_.to(target, ui::motion::lyrics, ui::Ease::Standard);
    const float off = scroll_.value();

    c.pushClip(area);
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        Rect lr = lineRects_[i].offset(0, area.y - off);
        lineRects_[i] = lr;
        if (lr.bottom() < area.y || lr.y > area.bottom()) {
            lines_[i].reset();   // scrolled away: drop its layout (no-op when it has none)
            continue;
        }
        Color colr;
        if (!synced) colr = col.fgSecondary;
        else if (i == active_) colr = col.fgPrimary;
        else {
            const int dist = std::abs(i - active_);
            const float a = i < active_ ? 0.35f : 0.22f;
            colr = col.fgPrimary.withAlpha(dist >= 3 ? a * 0.6f : a);
        }
        if (i == hoverLine_ && synced && i != active_) colr = col.fgPrimary.withAlpha(0.7f);
        if (i == active_ && synced) c.text(activeText_, lr, colr);
        else c.text(lines_[i], lr, colr);
    }
    c.popClip();
    // Edge fades.
    c.fillLinearGradient({area.x, area.y, area.w, 40}, {0, area.y}, {0, area.y + 40}, col.bgBase.withAlpha(0.0f), col.bgBase.withAlpha(0.f));
    if (synced && p->isPlaying()) {
        if (auto* w = window()) w->invalidateAfter(50);
    }
    (void)acc;
}

void NowPlayingView::paint(Canvas& c) {
    // Opening the view is a burst of first-time GPU work (blurred backdrop, display-size glyphs, new shader variants)
    // whose scratch stays in Direct2D's / the driver's caches next to what the pages behind it cached. Once the view
    // has settled, hand that back like the startup trim does (App::housekeeping): measured -10..13 MB private bytes
    // while it stays open. After the frame: the device must not be trimmed mid-draw.
    if (!trimmed_ && ui::frame::realNow() - openedAt_ > 5000) {
        trimmed_ = true;
        Dispatcher::post([] { gfx::Device::get().trim(); });
    }
    const Rect r = rect();
    const auto& col = colors();
    const auto* p = ctx().player;
    c.pushClip(r);
    paintBackdrop(c, r);
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
    const auto* t = p ? p->current() : nullptr;
    if (t) {
        // Hero art with glow + depth shadow.
        paintArtShadow(c);   // nowPlayingArtShadow
        drawArtwork(c, t->album.images, artRect_, 2);
        float y = artRect_.bottom() + 28;
        const float w = artRect_.w + 40;
        const float th = title_.measure(w).h;
        c.text(title_, {artRect_.x, y, w, th}, col.fgPrimary);
        y += th + 8;
        c.text(subtitle_, {artRect_.x, y, w, 24}, col.fgSecondary, gfx::VAlign::Center);
        y += 34;
        // Like Spotube, no stream / source wording (bitrate, YouTube / Piped / Invidious): the duration and how
        // sure the match is, next to "Yanlış eşleşme?".
        std::wstring meta = ui::formatDuration(p->durationMs());
        if (auto m = p->currentMatch()) {
            const int pct = static_cast<int>(std::round(std::clamp(m->score, 0.0, 100.0)));
            meta = i18n::format(tr(L"{} · EŞLEŞME %{}"), {meta, std::to_wstring(pct)});   // "03:42 · EŞLEŞME %87"
        }
        meta_.setText(meta);
        const float mw = std::ceil(meta_.measure().w);
        c.text(meta_, {artRect_.x, y, mw + 1, 16}, col.fgTertiary, gfx::VAlign::Center);
        const std::wstring wrongMatch = tr(L"Yanlış eşleşme?");
        auto wl = gfx::makeLayout(wrongMatch, type::caption, 400);
        DWRITE_TEXT_METRICS wm{};
        wl->GetMetrics(&wm);
        matchRect_ = {artRect_.x + mw + 14, y - 2, std::ceil(wm.widthIncludingTrailingWhitespace) + 4, 20};
        c.text(wrongMatch, type::caption, matchRect_, matchHover_ ? accent().base : col.fgSecondary,
               gfx::TextAlign::Leading, gfx::VAlign::Center);
    }
    paintLyrics(c, lyricsRect_);
    c.popTransform();
    paintChildren(c);
    c.popClip();
}

int NowPlayingView::lineAt(gfx::Point p) const {
    for (int i = 0; i < static_cast<int>(lineRects_.size()); ++i)
        if (lineRects_[i].contains(p)) return i;
    return -1;
}

bool NowPlayingView::onWheel(float dy, float, const ui::MouseEvent& e) {
    const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
    if (!lyricsRect_.contains(lp) || lines_.empty()) return false;
    manualOffset_ = (ui::frame::realNow() < manualUntil_ ? manualOffset_ : scroll_.value()) - dy * 90.f;
    manualUntil_ = ui::frame::realNow() + 3000;   // snap back to the active line after 3 s
    invalidate();
    return true;
}

bool NowPlayingView::onMouseDown(const ui::MouseEvent& e) {
    const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
    if (matchRect_.contains(lp)) {
        if (const auto* t = ctx().player->current(); t && ctx().showMatchPicker) ctx().showMatchPicker(*t);
        return true;
    }
    if (state_ != LyricsState::Synced) return false;
    const int i = lineAt(lp);
    if (i >= 0 && lyrics_.lines[i].timeMs >= 0) {
        ctx().player->seek(lyrics_.lines[i].timeMs);
        manualUntil_ = 0;
        return true;
    }
    return false;
}

void NowPlayingView::onMouseMove(const ui::MouseEvent& e) {
    const gfx::Point lp{e.pos.x - rect().x, e.pos.y - rect().y};
    const int i = state_ == LyricsState::Synced ? lineAt(lp) : -1;
    const bool mh = matchRect_.contains(lp);
    if (i != hoverLine_ || mh != matchHover_) {
        hoverLine_ = i;
        matchHover_ = mh;
        invalidate();
    }
}

} // namespace st::app
