#include "app/LyricsFullscreen.h"

#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/InternetRadio.h"
#include "app/LyricsPicker.h"
#include "app/LyricsService.h"
#include "app/LyricsShare.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "player/Player.h"
#include "ui/Acrylic.h"
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
    if (catalog::isPodcastId(t->id)) {   // episodes show their notes in Now Playing instead
        toast(tr(L"Podcast bölümlerinde şarkı sözü gösterilmez"));
        return;
    }
    w->pushOverlay(std::make_unique<LyricsFullscreen>(), true);
}

bool LyricsFullscreen::isOpen() { return g_view != nullptr; }

void LyricsFullscreen::trackChanged() {
    if (!g_view) return;
    const auto* t = ctx().player ? ctx().player->current() : nullptr;
    if (t && catalog::isPodcastId(t->id)) {   // the queue moved on to an episode: nothing to sing along to
        g_view->close();
        return;
    }
    g_view->load();
}

LyricsFullscreen::LyricsFullscreen()
    : title_({}, type::sectionTitle), artist_({}, type::secondary), time_({}, type::monoDuration), label_({}, type::monoLabel) {
    g_view = this;
    drag_ = add<ui::Widget>();
    drag_->isDragRegion = true;
    translate_ = add<ui::Button>(ui::ButtonKind::Chip, tr(L"Çeviri"));
    translate_->setTooltip(tr(L"Her satırın çevirisini altında göster"));
    translate_->onClick = [this] {
        translation_.toggle();
        showControls();
    };
    translation_.onChange = [this] {
        rebuildSubLines();
        syncControls();
    };
    romanize_ = add<ui::Button>(ui::ButtonKind::Chip, tr(L"Romanizasyon"));
    romanize_->setTooltip(tr(L"Her satırın Latin harfleriyle okunuşunu altında göster"));
    romanize_->onClick = [this] {
        romanization_.toggle();
        showControls();
    };
    romanization_.onChange = [this] {
        rebuildSubLines();
        syncControls();
    };
    earlier_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "minus");
    earlier_->setTooltip(commands::keyTip(tr(L"Sözleri daha erken göster"), "lyrics-earlier"));
    earlier_->onClick = [this] { shiftOffset(-kLyricsOffsetStepMs); };
    offset_ = add<ui::Button>(ui::ButtonKind::Ghost, formatLyricsOffset(0));
    offset_->setTooltip(tr(L"Söz zamanlamasını sıfırla"));
    offset_->onClick = [this] {
        if (!trackId_.empty()) setLyricsOffset(trackId_, 0);
        syncControls();
    };
    later_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "plus");
    later_->setTooltip(commands::keyTip(tr(L"Sözleri daha geç göster"), "lyrics-later"));
    later_->onClick = [this] { shiftOffset(kLyricsOffsetStepMs); };
    close_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "close");
    close_->setTooltip(tr(L"Kapat") + std::wstring(L"\tEsc"));   // the key as the tooltips show keys
    close_->onClick = [this] { close(); };
    share_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "share");
    share_->setTooltip(tr(L"Sözleri paylaş"));
    share_->onClick = [this] { openLyricsShare(lyrics_, active_, translation_); };
    // "Yanlış sözler mi?": the picker over this view; a new pick loads here and in Now Playing.
    picker_ = add<ui::Button>(ui::ButtonKind::Icon, L"", "search");
    picker_->setTooltip(tr(L"Yanlış sözler mi?"));
    picker_->onClick = [this] {
        if (const auto* t = ctx().player ? ctx().player->current() : nullptr) showLyricsPicker(*t, lyrics_.lrclibId);
    };
    onLyricsChoiceChanged(subs_.ref(), [this](const std::string& id) {
        if (id == trackId_) load();
    });
    open_.to(1, ui::overlay::duration(ui::motion::medium), ui::Ease::Decelerate);
    controls_.snap(1);
    lastMove_ = ui::frame::realNow();
    load();
}

LyricsFullscreen::~LyricsFullscreen() {
    if (g_view == this) g_view = nullptr;
}

void LyricsFullscreen::close() {
    if (closing_) return;
    closing_ = true;
    if (g_view == this) g_view = nullptr;
    life_.renew();
    exit_.snap(1);
    exit_.to(0, ui::overlay::duration(140), ui::Ease::Accelerate);   // the dialogs' close
    if (auto* w = window()) w->removeOverlay(this);
}

// ---- Lyrics ----------------------------------------------------------------------------------------------------

void LyricsFullscreen::load() {
    life_.renew();
    pick_ = {};
    lyrics_ = {};
    lines_.clear();
    shapes_.clear();
    lineH_.clear();
    lineRects_.clear();
    motion_.reset(0);
    translation_.clear();
    romanization_.clear();
    trans_.clear();
    roman_.clear();
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
    pick_ = q.choice;
    syncControls();
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
              shapes_.assign(lines_.size(), {});
              lineH_.assign(lines_.size(), -1.f);
              motion_.reset(lines_.size());
              // Offered when the lyrics are in another language / script (found out off the UI thread; onChange
              // follows).
              if (state_ == State::Synced || state_ == State::Plain) {
                  translation_.setLyrics(lyrics_, trackId_);
                  romanization_.setLyrics(lyrics_, trackId_);
              }
              syncControls();
              requestLayout();
              invalidate();
          });
}

void LyricsFullscreen::rebuildSubLines() {
    auto lines = [this](auto& source) {
        std::vector<std::wstring> v;
        v.reserve(lines_.size());
        for (size_t i = 0; i < lines_.size(); ++i) v.push_back(toWide(source.line(i)));
        return v;
    };
    if (romanization_.shown()) roman_.set(lines(romanization_), subStyle_, gfx::TextAlign::Center);
    else roman_.clear();
    if (translation_.shown()) trans_.set(lines(translation_), subStyle_, gfx::TextAlign::Center);
    else trans_.clear();
    invalidate();
}

float LyricsFullscreen::subHeight(int i, float width) {
    const float gap = std::round(style_.size * 0.16f);
    const float r = roman_.height(static_cast<size_t>(i), width), t = trans_.height(static_cast<size_t>(i), width);
    return (r > 0 ? gap + r : 0) + (t > 0 ? gap + t : 0);
}

void LyricsFullscreen::syncControls() {
    const bool synced = state_ == State::Synced;
    translate_->setVisible((synced || state_ == State::Plain) && translation_.offered());
    translate_->setActive(translation_.on());
    romanize_->setVisible((synced || state_ == State::Plain) && romanization_.offered());
    romanize_->setActive(romanization_.on());
    earlier_->setVisible(synced);
    offset_->setVisible(synced);
    later_->setVisible(synced);
    share_->setVisible(synced || state_ == State::Plain);
    picker_->setVisible(Settings::get().lyricsEnabled && (synced || state_ == State::Plain || state_ == State::Instrumental ||
                                                          state_ == State::Missing));
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
        // Translations and romanizations at half the size, lighter.
        subStyle_ = {std::round(size * 0.5f), 600, 1.2f, -0.01f};
        trans_.setStyle(subStyle_);
        roman_.setStyle(subStyle_);
    }
    const float pad = std::max(48.f, r.w * 0.08f);
    const float w = std::min(r.w - pad * 2, 1200.f);
    lyricsArea_ = {std::round((r.w - w) * 0.5f), kTopH, std::round(w), std::max(100.f, r.h - kTopH - kFooterH)};

    const float cy = 32, bs = 32;
    float x = r.w - pad - bs;   // the right edge of the label / time column, like the footer's time
    close_->setRect({x, cy - bs / 2, bs, bs});
    x -= 16;
    if (share_->visible()) {
        x -= bs;
        share_->setRect({x, cy - bs / 2, bs, bs});
        x -= 12;
    }
    if (later_->visible()) {
        x -= bs;
        later_->setRect({x, cy - bs / 2, bs, bs});
        const float ow = std::max(72.f, offset_->naturalWidth());
        x -= ow + 4;
        offset_->setRect({x, cy - 16, ow, 32});
        x -= bs + 4;
        earlier_->setRect({x, cy - bs / 2, bs, bs});
        x -= 16;
    }
    if (translate_->visible()) {
        const float tw = std::ceil(translate_->naturalWidth());
        x -= tw;
        translate_->setRect({x, cy - 14, tw, 28});
        x -= 12;
    }
    if (romanize_->visible()) {
        const float tw = std::ceil(romanize_->naturalWidth());
        x -= tw;
        romanize_->setRect({x, cy - 14, tw, 28});
        x -= 12;
    }
    if (picker_->visible()) {
        x -= bs;
        picker_->setRect({x, cy - bs / 2, bs, bs});
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
        if (!shapes_[i].built()) lines_[i].reset();   // measured only: a line off screen keeps no layout
    }
    return lineH_[i];
}

int LyricsFullscreen::lineAt(gfx::Point p) const {
    for (int i = 0; i < static_cast<int>(lineRects_.size()); ++i)
        if (lineRects_[i].contains(p) && lyricsArea_.contains(p)) return i;
    return -1;
}

// ---- Painting --------------------------------------------------------------------------------------------------

// Now Playing's backdrop: the cover's colours drifting slowly while it plays (app/CoverBackdrop), kept in the band where
// the text keeps its contrast. Returns the delay until its next drift step (0 = still).
double LyricsFullscreen::paintBackdrop(Canvas& c, const Rect& r) {
    const auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    return backdrop_.paint(c, r, t, p && p->isPlaying());
}

// The sung part of the active line: per word when the lyrics carry word times, else a sweep across the line lasting
// until the next line (capped by the line's length, so a long instrumental gap doesn't crawl). `fill` gets, per visual
// row of the line, the layout x the fill has reached (lyricsview::drawFill: all of a row / none of it as +-1e9).
void LyricsFullscreen::sweepRows(int i, float width, int posMs, std::vector<float>& fill) {
    fill.clear();
    const auto& line = lyrics_.lines[i];
    auto& text = lines_[i];
    auto* layout = text.layout(width);
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
    // Boxes come one per visual row (left to right); a row's fill is where the sweep has reached in it.
    UINT32 rows = 0;
    layout->GetLineMetrics(static_cast<DWRITE_LINE_METRICS*>(nullptr), 0, &rows);
    std::vector<DWRITE_LINE_METRICS> metrics(rows);
    if (rows == 0 || FAILED(layout->GetLineMetrics(metrics.data(), rows, &rows))) return;
    fill.assign(rows, -1e9f);
    float acc = 0;
    for (const auto& b : boxes) {
        float top = 0;
        size_t row = 0;
        for (; row + 1 < rows && top + metrics[row].height * 0.5f < b.top; ++row) top += metrics[row].height;
        const float cover = std::clamp(sweep - acc, 0.f, b.width);
        if (cover >= b.width - 0.01f) fill[row] = std::max(fill[row], 1e9f);
        else if (cover > 0) fill[row] = std::max(fill[row], b.left + cover);
        acc += b.width;
    }
}

void LyricsFullscreen::paintLyrics(Canvas& c, const Rect& area) {
    const auto& col = colors();
    auto centered = [&](const std::wstring& big, const std::wstring& note) {
        c.text(big, type::headline, {area.x, area.cy() - 40, area.w, 34}, col.fgSecondary, gfx::TextAlign::Center);
        if (!note.empty())
            c.text(note, type::monoLabel, {area.x, area.cy() + 6, area.w, 16}, backdrop_.labelColor(), gfx::TextAlign::Center);
    };
    switch (state_) {
    case State::None: return;
    case State::Loading:
        for (int i = 0; i < 5; ++i) {
            const float w = area.w * (0.35f + 0.1f * ((i * 7) % 4));
            ui::paintSkeleton(c, this, {area.cx() - w / 2, area.cy() - 110 + i * 52.f, w, 28}, gfx::radii().badge);
        }
        return;
    case State::Missing:
        if (pick_.kind == lyrics::Choice::Kind::None && Settings::get().lyricsEnabled)
            centered(tr(L"Bu şarkıda sözleri kapattın."), toUpperTr(tr(L"Senin seçimin")));
        else
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
    translation_.sync();   // the setting / target language changed in Now Playing or Ayarlar
    romanization_.sync();
    // The sung line grows to full size and brightens, the one before eases back (app/LyricsRender).
    static constexpr lyricsview::Dim kDim{0.32f, 0.24f, 0.6f};
    constexpr float kRestScale = 0.9f;   // lines not being sung
    motion_.setActive(synced ? active_ : -1, kDim);
    const bool emphasis = synced;

    // Heights on the pixel grid at rest (a resting line's top lands on a whole pixel), eased in between.
    const float gap = lyricsview::snap(c, style_.size * 0.55f);
    auto mainH = [&](int i, float e) {
        const float h = lineHeight(i, area.w);
        if (!emphasis) return h;
        return ui::lerp(lyricsview::ceilPx(c, h * kRestScale), lyricsview::ceilPx(c, h), e);
    };
    lineRects_.resize(lines_.size());
    float y = 0, targetY = 0, activeTop = 0, activeH = 0;
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        const float sub = subHeight(i, area.w);
        const float h = mainH(i, motion_.emphasis(i)) + sub;   // a line's rect holds its romanization and translation
        lineRects_[i] = {area.x, y, area.w, h};
        // Where the sung line will be once the change has eased out: the scroll heads there.
        const float th = mainH(i, motion_.emphasisTarget(i)) + sub;
        if (i == active_) {
            activeTop = targetY;
            activeH = th;
        }
        y += h + gap;
        targetY += th + gap;
    }
    const float total = y;
    const float lo = -area.h * 0.5f, hi = std::max(0.f, total - area.h * 0.5f);
    scroll_.setBounds(lo, hi);
    if (ui::frame::realNow() >= manualUntil_) {   // the wheel's offset holds for 3 s, then the active line leads again
        float target = scroll_.target();
        if (synced && active_ >= 0) target = lyricsview::snap(c, activeTop + activeH * 0.5f - area.h * 0.45f);
        else if (synced) target = lyricsview::snap(c, -area.h * 0.35f);
        target = std::clamp(target, lo, hi);
        if (std::abs(target - scroll_.target()) > 0.5f) scroll_.glideTo(target, ui::ScrollFeel::Follow);
    }
    const float offY = scroll_.value();

    // The top and bottom fade out: lines are clipped to a band a little taller than the area.
    const Rect band{0, area.y - 24, rect().w, area.h + 48};
    const Color accentC = accent().base;
    const float feather = style_.size * 0.6f;
    c.pushClip(band);
    for (int i = 0; i < static_cast<int>(lines_.size()); ++i) {
        const size_t li = static_cast<size_t>(i);
        Rect lr = lineRects_[i].offset(0, area.y - offY);
        lineRects_[i] = lr;
        if (lr.bottom() < band.y || lr.y > band.bottom()) {
            lines_[i].reset();   // scrolled away: drop its layout and shape
            shapes_[i].reset();
            trans_.release(li);
            roman_.release(li);
            continue;
        }
        // Lines near the band's edges fade out.
        const float edge = std::min(lr.cy() - band.y, band.bottom() - lr.cy());
        const float fade = std::clamp(edge / 120.f, 0.f, 1.f);
        const float e = emphasis ? motion_.emphasis(li) : 1.f;
        Color colr = synced ? col.fgPrimary.withAlpha(motion_.alpha(li)) : col.fgSecondary;
        if (i == hoverLine_ && synced && i != active_) colr = col.fgPrimary.withAlpha(std::max(0.7f, colr.a));
        colr = colr.mulAlpha(fade);
        const float scale = emphasis ? ui::lerp(kRestScale, 1.f, e) : 1.f;
        const lyricsview::Place at{{lr.x, lr.y}, scale, {area.w * 0.5f, 0}};
        // The karaoke fill: the sung line fills with the accent behind a soft edge; a line handing over keeps its
        // fill while it eases back, the accent fading with it.
        if (synced && e > 0.01f && !ui::motion::reduced()) {
            if (i == active_) sweepRows(i, area.w, pos + 150, fill_);
            else fill_.assign(64, i < active_ ? 1e9f : -1e9f);   // sung before / after a seek back
            lyricsview::drawFill(c, lines_[i], shapes_[i], area.w, at, fill_, Color::lerp(colr, accentC.mulAlpha(fade), e), colr,
                                 feather);
        } else {
            lyricsview::draw(c, lines_[i], shapes_[i], area.w, at, colr);
        }
        // Romanization, then translation, under the line (full size, not scaled).
        float sy = lr.y + mainH(i, e);
        const float subGap = std::round(style_.size * 0.16f);
        if (roman_.has(li)) {
            sy += subGap;
            roman_.draw(c, li, area.w, {{lr.x, sy}}, colr.mulAlpha(0.72f));
            sy += roman_.height(li, area.w);
        }
        if (trans_.has(li)) {
            sy += subGap;
            trans_.draw(c, li, area.w, {{lr.x, sy}}, colr.mulAlpha(0.62f));
        }
    }
    c.popClip();
    // The fill's soft edge moves smoothly at ~60 frames a second while it plays (each one a few draw calls).
    if (synced && p && p->isPlaying()) {
        if (auto* w = window()) w->invalidateAfter(ui::motion::reduced() ? 250 : 16);
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
        c.text(time_, {r.right() - pad - tw, y + 8, tw, 18}, backdrop_.labelColor(), gfx::VAlign::Center);
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
    std::wstring label = toUpperTr(tr(L"Şarkı sözleri"));
    if ((state_ == State::Synced || state_ == State::Plain) && !lyrics_.source.empty())
        label += L" · " + lyricsSourceLabel(lyrics_.source);
    if (state_ == State::Plain) label += L" · " + toUpperTr(tr(L"Senkronize değil"));
    if (pick_.kind != lyrics::Choice::Kind::Auto && pick_.kind != lyrics::Choice::Kind::None &&
        (state_ == State::Synced || state_ == State::Plain || state_ == State::Instrumental))
        label += L" · " + toUpperTr(tr(L"Senin seçimin"));
    label_.setText(label);
    const float pad = std::max(48.f, r.w * 0.08f);
    float controlsX = close_->rect().x;   // the leftmost control shown
    for (auto* b : {share_, later_, offset_, earlier_, translate_, romanize_, picker_})
        if (b->visible()) controlsX = std::min(controlsX, b->rect().x);
    const float lw = std::max(40.f, std::min(r.w - pad - 260, controlsX - 16 - pad));
    c.text(label_, {pad, 24, lw, 16}, backdrop_.labelColor(), gfx::VAlign::Center);
    // The translation's and the romanization's state under it: "ÇEVRİLİYOR…", "ÇEVİRİ ALINAMADI", what is shown.
    std::wstring note = romanization_.note();
    if (const std::wstring t = translation_.note(); !t.empty()) note = note.empty() ? t : note + L"  ·  " + t;
    if (!note.empty())
        c.text(note, type::monoLabel, {pad, 42, lw, 14}, backdrop_.labelColor(), gfx::TextAlign::Leading, gfx::VAlign::Center);
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
    const float t = open_ * (closing_ ? std::clamp(exit_.value(), 0.f, 1.f) : 1.f);
    if (t <= 0.001f) return;
    if (t < 0.999f) c.pushOpacity(t);
    const double drift = paintBackdrop(c, r);
    paintLyrics(c, lyricsArea_);
    if (drift > 0)
        if (auto* win = window()) win->invalidateAfter(drift);
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
    if (lines_.empty() || dy == 0) return true;
    scroll_.wheel(dy, 110.f, lyricsArea_.h * 0.8f);   // glides on from wherever the follow motion is
    manualUntil_ = ui::frame::realNow() + 3000;   // snaps back to the active line after 3 s
    if (auto* w = window()) w->invalidateAfter(3050);
    invalidate();
    return true;
}

} // namespace st::app
