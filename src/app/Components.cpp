#include "app/Components.h"

#include "app/Blacklist.h"
#include "app/Display.h"
#include "app/DragDrop.h"
#include "app/LyricsFullscreen.h"
#include "app/SmartShuffle.h"
#include "app/Router.h"
#include "app/Together.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Layout.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

// ---------------------------------------------------------------------------------------------------

namespace {
// placeholders/*.svg redrawn from the palette for every theme but the default dark one (the SVGs bake its bg.overlay
// #211D19 and fg.tertiary #6B645B): same 256-unit geometry, bg.sunken paper (bg.overlay on dark) + fg.tertiary strokes.
void drawLightPlaceholder(Canvas& c, Placeholder ph, const Rect& r, float radius) {
    const auto& col = colors();
    c.fillRounded(r, radius, gfx::isLightTheme() ? col.bgSunken : col.bgOverlay);
    const float s = std::min(r.w, r.h) / 256.f;
    const gfx::Point o{r.cx() - 128 * s, r.cy() - 128 * s};
    auto P = [&](float x, float y) { return gfx::Point{o.x + x * s, o.y + y * s}; };
    const Color ink = col.fgTertiary;
    const float w2 = std::max(1.f, 2 * s), w1 = std::max(0.75f, s);
    switch (ph) {
    case Placeholder::Album:
        c.strokeCircle(P(128, 128), 72 * s, ink, w2);
        c.strokeCircle(P(128, 128), 48 * s, ink, w1);
        c.fillCircle(P(128, 128), 16 * s, ink);
        break;
    case Placeholder::Artist:
        c.strokeCircle(P(128, 100), 36 * s, ink, w2);
        c.arc(P(128, 212), 72 * s, 270.f, 180.f, ink, w2);   // shoulders: (56,212) over the top to (200,212)
        break;
    case Placeholder::Playlist:
        c.line(P(64, 88), P(160, 88), ink, w2);
        c.line(P(64, 128), P(160, 128), ink, w2);
        c.line(P(64, 168), P(120, 168), ink, w2);
        c.icon("play", {o.x + 166 * s, o.y + 146 * s, 44 * s, 44 * s}, ink);   // the SVG's small play triangle
        break;
    case Placeholder::Folder: break;   // drawn by drawArtwork (no SVG)
    }
}
} // namespace

void drawArtwork(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius, Placeholder ph,
                 bool circle, Priority prio) {
    const int px = static_cast<int>(std::ceil(std::max(r.w, r.h) * c.scale()));
    ID2D1Bitmap1* bmp = nullptr;
    if (const auto* img = catalog::pickImage(images, px)) bmp = gfx::ImageCache::get().request(img->url, px, prio);
    if (bmp) {
        if (circle) c.imageCircle(bmp, r);
        else c.image(bmp, r, radius);
        return;
    }
    if (ph == Placeholder::Folder && !circle) {   // no placeholder SVG: the folder glyph on the placeholder surface
        c.fillRounded(r, radius, gfx::isLightTheme() ? colors().bgSunken : colors().bgOverlay);
        const float g = std::round(std::min(r.w, r.h) * 0.3f);
        c.icon("folder", r.center(g, g), colors().fgTertiary);
        return;
    }
    // Placeholder (placeholders/*.svg are full-bleed artwork in their own colors).
    const char* name = ph == Placeholder::Artist     ? "placeholders/artist-placeholder"
                       : ph == Placeholder::Playlist ? "placeholders/playlist-placeholder"
                                                     : "placeholders/album-placeholder";
    if (circle) {
        c.fillCircle({r.cx(), r.cy()}, r.w * 0.5f, gfx::isLightTheme() ? colors().bgSunken : colors().bgOverlay);
        c.icon("user", r.center(r.w * 0.35f, r.w * 0.35f), colors().fgTertiary);
    } else if (gfx::isLightTheme() || gfx::Theme::get().themeId() != "dark") {   // the SVGs are the default dark's colors
        drawLightPlaceholder(c, ph, r, radius);
    } else {
        c.fillRounded(r, radius, colors().bgOverlay);
        c.iconColored(name, r.center(r.w, r.h));
    }
}

void drawPlayingBars(Canvas& c, ui::Widget* owner, const Rect& box, Color color) {
    const auto* p = ctx().player;
    ui::paintEqualizer(c, owner, box, color, p && p->isPlaying() && !LyricsFullscreen::isOpen());
}

Color tileColor(std::string_view seed) {
    static const uint32_t palette[] = {0x3B2A1E, 0x1F2A22, 0x2A1E2A, 0x1C2433, 0x33261B, 0x1E2B2B, 0x2E1D1D, 0x26241C};
    uint32_t h = 2166136261u;
    for (char ch : seed) h = (h ^ static_cast<unsigned char>(ch)) * 16777619u;
    return Color::rgb(palette[h % std::size(palette)]);
}

Color tileFill(Color tone) {
    if (!gfx::isLightTheme()) return tone;
    const gfx::Hsl h = gfx::toHsl(tone);
    if (h.s < 0.05f) return colors().bgSunken;   // neutral tones (Metal): plain sunken paper
    // Same hue as the dark tone, pale and softly saturated: a tinted paper card under fg.primary text.
    return gfx::fromHsl({h.h, std::clamp(h.s * 1.1f + 0.08f, 0.18f, 0.42f), 0.86f}, tone.a);
}

// ---------------------------------------------------------------------------------------------------
// TrackSwap / FadingArt

namespace {
constexpr float kSwapShift = 10;   // DIP
}

bool TrackSwap::update(const player::Player* p) {
    const auto* t = p ? p->current() : nullptr;
    const std::string id = t ? t->id : std::string();
    const int index = p ? p->currentOrderIndex() : -1;
    const uint64_t gen = p ? p->queueGeneration() : 0;
    const bool animate = started_ && id != id_ && !id.empty() && !id_.empty();
    if (animate) {
        // Forward unless an earlier slot of the same queue plays (previous, a queue row above); repeat-all wrapping
        // from the last slot to the first still counts as forward, and back from the first to the last as previous.
        const int last = p ? static_cast<int>(p->order().size()) - 1 : -1;
        bool back = gen == gen_ && index >= 0 && index_ >= 0 && index < index_;
        if (gen == gen_ && index == 0 && index_ == last && last > 1) back = false;
        if (gen == gen_ && index_ == 0 && index == last && last > 1) back = true;
        dir_ = back ? -1.f : 1.f;
        t_.snap(0);
        t_.to(1, ui::motion::slow, ui::Ease::Linear);
    }
    started_ = true;
    id_ = id;
    index_ = index;
    gen_ = gen;
    return animate && t_.running();
}

TrackSwap::Layer TrackSwap::outgoing() const {
    const float t = t_.value();
    if (t >= 1.f) return {0, 0};
    const float u = std::clamp(t / 0.55f, 0.f, 1.f);
    return {1 - u, -dir_ * kSwapShift * ui::ease(ui::Ease::Accelerate, u)};
}

TrackSwap::Layer TrackSwap::incoming() const {
    const float u = std::clamp((t_.value() - 0.3f) / 0.7f, 0.f, 1.f);
    const float e = ui::ease(ui::Ease::Decelerate, u);
    return {e, dir_ * kSwapShift * (1 - e)};
}

float TrackSwap::progress() const { return ui::ease(ui::Ease::Standard, std::clamp(t_.value(), 0.f, 1.f)); }

void FadingArt::draw(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius) {
    const int px = static_cast<int>(std::ceil(std::max(r.w, r.h) * c.scale()));
    const auto* img = catalog::pickImage(images, px);
    const std::string url = img ? img->url : std::string();
    if (url != url_) {   // another cover: shown at once if it is cached, else faded in when it arrives
        url_ = url;
        waiting_ = false;
        in_.snap(1);
    }
    ID2D1Bitmap1* bmp = img ? gfx::ImageCache::get().request(img->url, px, Priority::High) : nullptr;
    if (!bmp) {
        waiting_ = !url.empty();
        drawArtwork(c, {}, r, radius);
        return;
    }
    if (waiting_) {
        waiting_ = false;
        in_.snap(0);
        in_.to(1, ui::motion::slow, ui::Ease::Standard);
    }
    const float a = in_.value();
    if (a < 0.999f) drawArtwork(c, {}, r, radius);
    c.image(bmp, r, radius, a);
}

// ---------------------------------------------------------------------------------------------------
// MediaCard

MediaCard::MediaCard(std::wstring title, std::wstring subtitle, std::vector<catalog::Image> images, Shape shape,
                     Placeholder ph)
    : title_(std::move(title), type::body.withSize(13)),
      subtitle_(std::move(subtitle), type::caption),
      images_(std::move(images)),
      shape_(shape),
      ph_(ph) {
    focusable = true;
    if (shape == Shape::Circle) {
        gfx::TextOptions o;
        o.align = gfx::TextAlign::Center;
        title_.setOptions(o);
        subtitle_.setOptions(o);
        title_.setStyle(type::body);
    }
}

float MediaCard::preferredHeight(float width) { return width + 10 + 18 + 4 + 16; }

Rect MediaCard::artRect() const {
    const Rect r = rect();
    return {r.x, r.y, r.w, r.w};
}

Rect MediaCard::playRect() const {
    const Rect a = artRect();
    return {a.right() - 12 - 48, a.bottom() - 12 - 48, 48, 48};
}

bool MediaCard::onMouseDown(const ui::MouseEvent& e) {
    if (e.button == ui::MouseButton::Right) {
        if (onContext) onContext(e.windowPos);
        return false;
    }
    if (e.button != ui::MouseButton::Left) return false;
    playPressed_ = onPlay && playRect().contains(e.pos);
    (playPressed_ ? playPressA_ : press_).to(1, ui::motion::microMs(80));
    invalidate();
    return true;
}

void MediaCard::onMouseUp(const ui::MouseEvent& e) {
    press_.to(0, ui::motion::microMs(160));
    playPressA_.to(0, ui::motion::microMs(160));
    playPressed_ = false;
    invalidate();
    if (!rect().contains(e.pos)) return;
    if (onPlay && playRect().contains(e.pos)) {
        pagetx::arm(cover());   // "#play" opens the page too
        onPlay();
    } else if (onOpen) {
        pagetx::arm(cover());
        onOpen();
    }
}

SharedCover MediaCard::cover() const {
    const Rect art = artRect();
    return pagetx::imageCover(*this, art, shape_ == Shape::Circle ? art.w * 0.5f : 2.f, images_);
}

bool MediaCard::coverFor(const std::string& key, SharedCover& out) {
    if (images_.empty()) return false;
    out = cover();
    return out.key == key;
}

void MediaCard::onMouseMove(const ui::MouseEvent& e) {
    const bool hot = onPlay && playRect().contains(e.pos);
    if (hot != playHot_) {
        playHot_ = hot;
        playHotA_.to(hot ? 1.f : 0.f, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
}

void MediaCard::onMouseLeave() {
    hover_.to(0, ui::motion::microMs(ui::motion::fast));
    playHot_ = false;
    playHotA_.to(0, ui::motion::microMs(ui::motion::fast));
}

bool MediaCard::onActivate() {
    if (!onOpen) return false;
    press_.snap(ui::motion::micro() ? 1.f : 0.f);
    press_.to(0, ui::motion::microMs(160));
    pagetx::arm(cover());
    const auto open = onOpen;   // navigating destroys this card (deferred), call a copy
    open();
    return true;
}

bool MediaCard::onKeyDown(const ui::KeyEvent& e) {
    if ((e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) && onContext) {
        const Rect a = toWindow(artRect());
        const auto context = onContext;
        context({a.x + 12, a.bottom() - 12});
        return true;
    }
    return false;
}

// Square cards: the whole card (art + title); artist cards: the round artwork.
Rect MediaCard::focusRect() const { return shape_ == Shape::Circle ? artRect() : rect(); }

void MediaCard::paint(Canvas& c) {
    const auto& col = colors();
    // Keyboard focus shows a square card's hover state (shadow + play button, no rise: the ring stays on the card);
    // round cards keep just the ring.
    const float pointer = hover_.value(), p = press_.value();
    const float h = std::max(pointer, shape_ == Shape::Square && keyboardFocused() ? 1.f : 0.f);
    const Rect art = artRect();
    const float lift = ui::card::lift(pointer, p);
    // Page transition: while this cover flies, the flight draws it (presence 0), then it lands here (0 -> 1).
    const float shown = pagetx::presence(this);
    c.pushTransform(D2D1::Matrix3x2F::Translation(0, lift));
    c.pushScale(ui::card::pressScale(p), {art.cx(), art.cy()});
    if (shown <= 0.f) {
        // nothing: the flight draws this cover
    } else if (shape_ == Shape::Circle) {
        const float d = std::min(art.w, 160.f + (art.w - 160.f));
        const Rect circle = art.center(d, d);
        ui::card::paintBack(c, circle, pointer, true);
        drawArtwork(c, images_, circle, 0, Placeholder::Artist, true);
        ui::card::paintEdge(c, circle, h, true);
    } else {
        ui::card::paintBack(c, art, h * shown);
        drawArtwork(c, images_, art, gfx::radii().art, ph_);
        ui::card::paintEdge(c, art, h);
        if (!badge_.empty()) {
            const float bw = std::ceil(badge_.measure().w) + 12;
            const Rect pill{art.x + 8, art.y + 8, bw, 16};
            c.fillPill(pill, accent().base);
            c.text(badge_, pill.inset(6, 0), accent().onAccent, gfx::VAlign::Center);
        }
        if (onPlay)
            ui::card::paintPlay(c, playRect(), {h, playHotA_.value(), playPressA_.value()}, accent().base, accent().hover,
                                accent().onAccent);
    }
    c.popTransform();
    const Rect tr{rect().x, art.bottom() + 10, rect().w, 18};
    c.text(title_, tr, col.fgPrimary, gfx::VAlign::Center);
    c.text(subtitle_, {rect().x, tr.bottom() + 4 - 2, rect().w, 16}, col.fgSecondary, gfx::VAlign::Center);
    c.popTransform();
}

// ---------------------------------------------------------------------------------------------------
// SectionHeader

SectionHeader::SectionHeader(std::wstring title, std::wstring meta, std::wstring link)
    : title_(std::move(title), type::title), meta_(std::move(meta), type::monoLabel) {
    hitTestVisible = false;
    if (!link.empty()) {
        link_ = add<ui::Button>(ui::ButtonKind::Link, std::move(link), "arrow-forward");
        link_->onClick = [this] {
            if (onLink) onLink();
        };
    }
}

void SectionHeader::layout() {
    if (!link_) return;
    const float w = link_->naturalWidth();
    link_->setRect({rect().w - w, 0, w, 36});
}

void SectionHeader::paint(Canvas& c) {
    const Rect r = rect();
    const float tw = std::ceil(title_.measure().w);
    c.text(title_, {r.x, r.y, tw + 1, 36}, colors().fgPrimary, gfx::VAlign::Center);
    if (!meta_.empty()) c.text(meta_, {r.x + tw + 14, r.y + 4, 200, 36}, colors().fgTertiary, gfx::VAlign::Center);
    c.hline(r.x, r.right(), r.y + 44, colors().hairDefault);
    paintChildren(c);
}

// ---------------------------------------------------------------------------------------------------
// Badges

namespace badge {

// The shared labels are never freed (static exit). "Engelli" = blocked (on the kara liste, the user's block list), not
// "disabled".
gfx::Text& blockedLabel() {
    static gfx::Text& t = *new gfx::Text(toUpperTr(tr(L"Engelli")), type::monoBadge);
    return t;
}
gfx::Text& localLabel() {
    static gfx::Text& t = *new gfx::Text(tr(L"YEREL"), type::monoBadge);
    return t;
}
gfx::Text& unavailableLabel() {
    static gfx::Text& t = *new gfx::Text(toUpperTr(tr(L"Kullanılamıyor")), type::monoBadge);
    return t;
}

float width(gfx::Text& text, bool icon) { return std::ceil(text.measure().w) + 10 + (icon ? 11.f : 0.f); }

float draw(Canvas& c, float x, float cy, gfx::Text& text, Tone tone, std::string_view icon) {
    const auto& col = colors();
    const auto& acc = accent();
    const float w = width(text, !icon.empty());
    const Rect r{std::round(x), std::round(cy - kH * 0.5f), w, kH};
    const Color ink = tone == Tone::Accent ? acc.base : col.fgSecondary;
    if (tone == Tone::Accent) c.fillRounded(r, gfx::radii().badge, acc.tint06);
    c.strokeRounded(r.inset(0.5f), gfx::radii().badge, tone == Tone::Accent ? acc.base.mulAlpha(0.55f) : col.hairStrong);
    float tx = r.x + 5;
    if (!icon.empty()) {
        c.icon(icon, {tx, r.cy() - 4, 8, 8}, ink);
        tx += 11;
    }
    // At the width it was measured at (gfx::Text keeps one layout per width: no relayout per frame).
    c.text(text, {tx, r.y, 100000.f, r.h}, ink, gfx::VAlign::Center);
    return w;
}

float drawExplicit(Canvas& c, float x, float cy) {
    static gfx::Text& e = *[] {
        gfx::TextOptions center;
        center.align = gfx::TextAlign::Center;
        return new gfx::Text(L"E", type::monoBadge, center);
    }();
    const Rect b{std::round(x), std::round(cy - 7), 14, 14};
    c.fillRounded(b, gfx::radii().badge, colors().fgPrimary.withAlpha(0.14f));
    c.text(e, b, colors().fgSecondary, gfx::VAlign::Center);
    return 14;
}

float drawDownloaded(Canvas& c, float x, float cy) {
    const float r = kMark * 0.5f;
    c.fillCircle({x + r, cy}, r, accent().base);
    c.icon("check", {x + 2, cy - 4, 8, 8}, accent().onAccent);
    return kMark;
}

} // namespace badge

// ---------------------------------------------------------------------------------------------------
// TrackTable

namespace {

using trackcols::Layout;

// A row action's slot: 28 DIP wide, a 16 DIP icon in its middle.
Rect slotRect(float x, const Rect& row) { return {x, row.cy() - 14, trackcols::kActionW, 28}; }
Rect slotIcon(float x, const Rect& row, float dx = 0) { return {x + 6 + dx, row.cy() - 8, 16, 16}; }

ui::ScrollView* scrollViewAbove(const ui::Widget* w) {
    for (ui::Widget* p = w->parent(); p; p = p->parent())
        if (auto* sv = dynamic_cast<ui::ScrollView*>(p)) return sv;
    return nullptr;
}

// Sorting from the header: text columns start ascending, time / count columns descending.
bool startsDescending(TrackTable::SortKey k) { return k == TrackTable::SortKey::Added || k == TrackTable::SortKey::Plays; }

} // namespace

TrackTable::TrackTable(Options opts) : opts_(opts) {
    focusable = true;
    // The "downloaded" marks follow the downloads (rows re-read theirs; no text is rebuilt).
    ctx().downloads.subscribe(life_.ref(), [this] { ++dlRev_; });
}

TrackTable::~TrackTable() {
    if (dragging_) dragdrop::cancel();   // the page went away mid-drag
}

void TrackTable::setTracks(std::vector<catalog::Track> tracks) {
    tracks_ = std::move(tracks);
    selected_.clear();
    anchor_ = rangeFrom_ = -1;
    nearEndSignaled_ = false;
    reorderRows_.clear();   // a drag under way can't move rows that were replaced (it still drops elsewhere)
    dropAt_ = -1;
    hasPlays_ = std::any_of(tracks_.begin(), tracks_.end(), [](const catalog::Track& t) { return t.listenCount > 0; });
    rebuildView();
    if (!tracks_.empty()) scheduleDemo();
}

// ---- Dev: SHADETUBE_LIST_DEMO -------------------------------------------------------------------------------------------
// "<step>;<step>…" run 900 ms after the first table of the run got its rows (screenshots of states a pointer can't hold):
// select=1,3-5 (display rows, 0-based; the cursor goes to the first), sort=<title|album|added|duration|plays>[:desc]
// (as a column header click: the page's sort label follows), columns=<Settings::trackColumns> (this run only),
// next=<rows> / enqueue=<rows> (Sonra çal / Sıraya ekle), queue (opens the queue panel), scroll=<dips> (the page),
// dragline=<row> (the selection shown as dragged to in front of that row: dimmed rows, the insertion line), fakeplays
// (made-up play counts, for the Dinlenme column).

namespace {
TrackTable* g_demoTable = nullptr;
Lifetime::Ref g_demoRef;
std::vector<std::string> g_demoSteps;
} // namespace

void TrackTable::scheduleDemo() {
    static bool done = false;
    if (done) return;
    done = true;
    char buf[1024] = {};
    if (!GetEnvironmentVariableA("SHADETUBE_LIST_DEMO", buf, sizeof buf)) return;
    std::string spec = buf;
    for (size_t p = 0; p <= spec.size();) {
        const size_t q = std::min(spec.find(';', p), spec.size());
        if (q > p) g_demoSteps.push_back(spec.substr(p, q - p));
        p = q + 1;
    }
    g_demoTable = this;
    g_demoRef = life_.ref();
    SetTimer(nullptr, 0, 900, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        if (g_demoRef.expired() || !g_demoTable) return;
        for (const auto& s : g_demoSteps) g_demoTable->runDemoStep(s);
    });
}

void TrackTable::runDemoStep(const std::string& step) {
    const size_t eq = step.find('=');
    const std::string action = step.substr(0, eq), arg = eq == std::string::npos ? std::string() : step.substr(eq + 1);
    auto rows = [&] {   // "1,3-5" -> display rows that exist
        std::vector<int> out;
        for (size_t p = 0; p < arg.size();) {
            const size_t q = std::min(arg.find(',', p), arg.size());
            const std::string part = arg.substr(p, q - p);
            const size_t dash = part.find('-');
            const int a = std::atoi(part.c_str()), b = dash == std::string::npos ? a : std::atoi(part.c_str() + dash + 1);
            for (int i = a; i <= b; ++i)
                if (i >= 0 && i < static_cast<int>(view_.size())) out.push_back(i);
            p = q + 1;
        }
        return out;
    };
    ST_LOG_INFO("tracktable", "list demo: {}", step);
    if (action == "select") {
        selected_.clear();
        const auto r = rows();
        for (int i : r) selected_.insert(view_[i]);
        if (!r.empty()) anchor_ = rangeFrom_ = r.front();
    } else if (action == "sort") {
        const bool desc = arg.find(":desc") != std::string::npos;
        const std::string k = arg.substr(0, arg.find(':'));
        const SortKey key = k == "title" ? SortKey::Title : k == "album" ? SortKey::Album : k == "added" ? SortKey::Added
                            : k == "duration" ? SortKey::Duration : k == "plays" ? SortKey::Plays : SortKey::None;
        sortBy(key, desc);
    } else if (action == "columns") {
        Settings::get().trackColumns = arg;
        requestLayout();
    } else if (action == "fakeplays") {   // made-up play counts (lists without them: the Dinlenme column's look)
        for (size_t i = 0; i < tracks_.size(); ++i) tracks_[i].listenCount = static_cast<int64_t>((i * 7919 + 13) % 4000) * 997;
        hasPlays_ = true;
        texts_.clear();
        requestLayout();
    } else if (action == "next" || action == "enqueue") {
        std::vector<catalog::Track> picked;
        for (int i : rows()) picked.push_back(tracks_[view_[i]]);
        if (action == "next")
            for (auto it = picked.rbegin(); it != picked.rend(); ++it) ctx().player->playNext(*it);
        else ctx().player->enqueue(picked);
    } else if (action == "queue") {
        ctx().toggleQueue(true);
    } else if (action == "scroll") {
        if (auto* sv = scrollViewAbove(this)) {
            sv->layout();
            sv->scrollTo(static_cast<float>(std::atof(arg.c_str())), false);
        }
    } else if (action == "dragline") {
        const auto r = rows();
        if (!r.empty() && !selected_.empty()) {
            for (int k : view_)
                if (selected_.contains(k)) reorderRows_.push_back(k);
            dragging_ = true;
            dropAt_ = r.front();
            dropY_.snap(rowRect(dropAt_).y);
        }
    }
    invalidate();
}

void TrackTable::selectRows(const std::vector<int>& rows) {
    selected_.clear();
    for (int i : rows)
        if (i >= 0 && i < static_cast<int>(tracks_.size())) selected_.insert(i);
    if (!rows.empty())
        for (int d = 0; d < static_cast<int>(view_.size()); ++d)
            if (view_[d] == rows.front()) anchor_ = rangeFrom_ = d;
    invalidate();
}

bool TrackTable::canReorder() const {
    return reorderable_ && onReorder && sortKey_ == SortKey::None && filter_.empty() &&
           std::none_of(tracks_.begin(), tracks_.end(), [](const catalog::Track& t) { return t.recommended; });
}

void TrackTable::appendTracks(std::vector<catalog::Track> tracks) {
    for (auto& t : tracks) {
        hasPlays_ = hasPlays_ || t.listenCount > 0;
        tracks_.push_back(std::move(t));
    }
    nearEndSignaled_ = false;
    rebuildView();
}

void TrackTable::setFilter(const std::wstring& text) {
    filter_ = foldForSearch(text);
    rebuildView();
}

void TrackTable::setSort(SortKey key, bool descending) {
    sortKey_ = key;
    sortDesc_ = descending;
    rebuildView();
}

void TrackTable::sortBy(SortKey key, bool descending) {
    setSort(key, descending);
    if (onSortChanged) {
        const auto fn = onSortChanged;   // the page may rebuild things
        fn(key, descending);
    }
}

void TrackTable::rebuildView() {
    view_.clear();
    view_.reserve(tracks_.size());
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        if (!filter_.empty()) {
            const auto& t = tracks_[i];
            const std::wstring hay = foldForSearch(toWide(t.name + " " + t.artistLine() + " " + t.album.name));
            if (hay.find(filter_) == std::wstring::npos) continue;
        }
        view_.push_back(i);
    }
    if (sortKey_ != SortKey::None) {
        // Text keys are folded once per row, not per comparison (10 000 rows sort in a few ms).
        std::vector<std::wstring> keys;
        if (sortKey_ == SortKey::Title || sortKey_ == SortKey::Album) {
            keys.resize(tracks_.size());
            for (int i : view_) keys[i] = foldForSearch(toWide(sortKey_ == SortKey::Title ? tracks_[i].name : tracks_[i].album.name));
        }
        const auto key = sortKey_;
        std::stable_sort(view_.begin(), view_.end(), [&](int a, int b) {
            const auto& x = tracks_[a];
            const auto& y = tracks_[b];
            switch (key) {
            case SortKey::Title:
            case SortKey::Album: return keys[a] < keys[b];
            case SortKey::Added: return x.addedAt < y.addedAt;
            case SortKey::Duration: return x.durationMs < y.durationMs;
            case SortKey::Plays: return x.listenCount < y.listenCount;
            default: return false;
            }
        });
        if (sortDesc_) std::reverse(view_.begin(), view_.end());
    }
    texts_.clear();
    hover_ = -1;
    hoverHit_ = Hit::None;
    hoverFade_.clear();
    requestLayout();
    invalidate();
}

std::vector<catalog::Track> TrackTable::displayedTracks() const {
    std::vector<catalog::Track> out;
    out.reserve(view_.size());
    for (int i : view_) out.push_back(tracks_[i]);
    return out;
}

TrackTable::RowMetrics TrackTable::rowMetrics() {
    // Sıkı: the two text lines (16.9 + 14.3 DIP line boxes) sit 4 DIP from the row's edges, the art 6.
    if (display::compactLists()) return {true, 40, 28, 10, 4, 17, 21, 15};
    return {false, gfx::metrics::trackRowH, gfx::metrics::trackArt, 12, 9, 20, 29, 16};
}

float TrackTable::preferredHeight(float) {
    if (tracks_.empty() && loadingRows_ == 0) return 0;   // nothing at all: the page's empty state stands alone
    return headerHeight() + (static_cast<float>(view_.size()) + static_cast<float>(loadingRows_)) * rowMetrics().h + 8;
}

uint32_t TrackTable::wantedColumns() const {
    const std::string& pick = Settings::get().trackColumns;
    if (pick != userColsSrc_ || !userColsRead_) {
        userColsSrc_ = pick;
        userCols_ = trackcols::parseColumns(pick);
        userColsRead_ = true;
    }
    uint32_t can = 0;
    if (opts_.showAlbum) can |= trackcols::Album;
    if (opts_.showAdded) can |= trackcols::Added;
    if (hasPlays_) can |= trackcols::Plays;
    return can & userCols_;
}

Layout TrackTable::columns() const {
    const Rect r = rect();
    return trackcols::layout({r.x, r.w, wantedColumns(), display::compactLists()});
}

Rect TrackTable::rowRect(int i) const {
    const Rect r = rect();
    const float h = rowMetrics().h;
    return {r.x, r.y + headerHeight() + i * h, r.w, h};
}

int TrackTable::rowAt(float y) const {
    const float top = rect().y + headerHeight();
    if (y < top) return -1;
    if (stickyY_ >= 0 && y >= stickyY_ && y < stickyY_ + kHeaderH) return -1;   // under the sticky header
    const int i = static_cast<int>((y - top) / rowMetrics().h);
    return i < static_cast<int>(view_.size()) ? i : -1;
}

TrackTable::RowText& TrackTable::rowText(int di) {
    if (const bool compact = display::compactLists(); compact != textsCompact_) {   // density changed: new styles
        texts_.clear();
        textsCompact_ = compact;
    }
    auto it = texts_.find(di);
    if (it == texts_.end()) {
        // Bound the cache: drop everything when it grows past a few screens of rows.
        if (texts_.size() > 160) texts_.clear();
        const auto& t = tracks_[view_[di]];
        RowText rt;
        rt.title = gfx::Text(toWide(t.name), textsCompact_ ? type::body.withSize(13) : type::body);
        rt.artist = gfx::Text(toWide(t.artistLine()), textsCompact_ ? type::tiny : type::caption);
        rt.album = gfx::Text(toWide(t.album.name), type::secondary);
        rt.added = gfx::Text(relativeTime(t.addedAt), type::caption);
        gfx::TextOptions right;
        right.align = gfx::TextAlign::Trailing;
        rt.dur = gfx::Text(ui::formatDuration(t.durationMs), type::monoDuration, right);
        rt.plays = gfx::Text(t.listenCount > 0 ? compactCount(t.listenCount) : std::wstring(), type::caption, right);
        wchar_t num[16];
        swprintf(num, 16, L"%02d", opts_.albumNumbering ? t.trackNumber : di + 1);
        rt.num = gfx::Text(num, type::monoDuration);
        rt.blocked = blacklist::isBlocked(t);
        rt.titleW = std::ceil(rt.title.measure().w);
        rt.artistW = rt.artist.empty() ? 0.f : std::ceil(rt.artist.measure().w);
        it = texts_.emplace(di, std::move(rt)).first;
    }
    RowText& rt = it->second;
    if (rt.dlRev != dlRev_) {   // the downloads changed since: re-read the mark only
        rt.dlRev = dlRev_;
        const auto& t = tracks_[view_[di]];
        rt.downloaded = opts_.markDownloaded && !t.id.empty() && ctx().downloads.isDownloaded(t.id);
    }
    return rt;
}

// ---- Header -----------------------------------------------------------------------------------------------------------

int TrackTable::headerColumnAt(float x) const {
    const Layout cl = columns();
    auto in = [x](float a, float w) { return w > 0 && x >= a - 4 && x < a + w + 4; };
    if (in(cl.title, cl.titleW)) return static_cast<int>(SortKey::Title);
    if (in(cl.album, cl.albumW)) return static_cast<int>(SortKey::Album);
    if (in(cl.added, cl.addedW)) return static_cast<int>(SortKey::Added);
    if (in(cl.plays, cl.playsW)) return static_cast<int>(SortKey::Plays);
    if (x >= cl.dur - 8 && x < cl.more + trackcols::kActionW) return static_cast<int>(SortKey::Duration);
    return -1;
}

void TrackTable::paintHeader(Canvas& c, const Rect& r) {
    const auto& col = colors();
    const auto& acc = accent();
    const Layout cl = columns();
    const float y = r.y;
    if (!hdr_.made) {   // the labels are laid out once (the header paints every frame while it is stuck)
        hdr_.made = true;
        hdr_.num = gfx::Text(L"#", type::monoLabel);
        hdr_.title = gfx::Text(toUpperTr(tr(L"Başlık")), type::monoLabel);
        hdr_.album = gfx::Text(toUpperTr(tr(L"Albüm")), type::monoLabel);
        hdr_.added = gfx::Text(toUpperTr(tr(L"Eklendi")), type::monoLabel);
        hdr_.plays = gfx::Text(toUpperTr(tr(L"Dinlenme")), type::monoLabel);
        hdr_.dur = gfx::Text(toUpperTr(tr(L"Süre")), type::monoLabel);
        hdr_.count = gfx::Text({}, type::monoLabel);
    }
    auto label = [&](gfx::Text& text, float x, float w, SortKey key, bool rightAlign = false) {
        if (w <= 0) return;
        const bool active = sortKey_ == key && key != SortKey::None;
        const bool hot = key != SortKey::None && headerHot_ == static_cast<int>(key);
        const Color ink = active ? col.fgPrimary : hot ? col.fgSecondary : col.fgTertiary;
        const float natural = std::ceil(text.measure().w);
        const float tw = std::min(w, natural);
        const float tx = rightAlign ? x + w - tw : x;
        // Drawn at the width it was measured at when it fits (one layout per gfx::Text: no relayout per frame).
        c.text(text, {tx, y, natural <= w ? 100000.f : w, kHeaderH}, ink, gfx::VAlign::Center);
        if (active) {   // the direction: a thin chevron beside the label (left of a right-aligned one)
            const float ax = rightAlign ? tx - 14 : tx + tw + 4;
            c.icon(sortDesc_ ? "chevron-down" : "chevron-up", {ax, y + kHeaderH * 0.5f - 5, 10, 10}, acc.base);
        }
    };
    label(hdr_.num, cl.num, cl.numW, SortKey::None);
    label(hdr_.title, cl.title, cl.titleW, SortKey::Title);
    if (multiSelect()) {   // several rows picked: how many, after the title column's label
        const auto n = static_cast<int64_t>(selected_.size());
        if (n != hdr_.countN) {
            hdr_.countN = n;
            hdr_.count.setText(L"· " + toUpperTr(i18n::plural(L"{} seçili", n)));
        }
        const float x = cl.title + std::ceil(hdr_.title.measure().w) + (sortKey_ == SortKey::Title ? 22.f : 10.f);
        const float room = std::max(0.f, cl.title + cl.titleW - x);
        c.text(hdr_.count, {x, y, std::ceil(hdr_.count.measure().w) <= room ? 100000.f : room, kHeaderH}, acc.base,
               gfx::VAlign::Center);
    }
    if (cl.albumW > 0 && !opts_.albumNumbering) label(hdr_.album, cl.album, cl.albumW, SortKey::Album);
    if (cl.addedW > 0) label(hdr_.added, cl.added, cl.addedW, SortKey::Added);
    if (cl.playsW > 0) label(hdr_.plays, cl.plays, cl.playsW, SortKey::Plays, true);
    label(hdr_.dur, cl.dur, cl.durW, SortKey::Duration, true);
    c.hline(r.x, r.right(), y + kHeaderH, col.hairDefault);
}

void TrackTable::headerClick(float x) {
    const int k = headerColumnAt(x);
    if (k < 0) return;
    const auto key = static_cast<SortKey>(k);
    // Cycle: the column's first direction -> the other -> the list's own order.
    const bool first = startsDescending(key);
    if (sortKey_ != key) sortBy(key, first);
    else if (sortDesc_ == first) sortBy(key, !first);
    else sortBy(SortKey::None, false);
}

bool TrackTable::headerPress(gfx::Point windowPos, bool right) {
    if (!opts_.showHeader) return false;
    if (right) showColumnsMenu(windowPos);
    else headerClick(fromWindow(windowPos).x);
    return true;
}

void TrackTable::headerHover(gfx::Point windowPos) {
    const int k = headerColumnAt(fromWindow(windowPos).x);
    if (k != headerHot_) {
        headerHot_ = k;
        invalidate();
    }
}

void TrackTable::headerLeave() {
    if (headerHot_ < 0) return;
    headerHot_ = -1;
    invalidate();
}

// The column header's menu: the optional columns this list has (the choice is the user's, for every list).
void TrackTable::showColumnsMenu(gfx::Point windowPos) {
    const uint32_t user = trackcols::parseColumns(Settings::get().trackColumns);
    auto toggle = [ref = life_.ref(), this](uint32_t col) {
        auto& s = Settings::get();
        s.trackColumns = trackcols::formatColumns(trackcols::parseColumns(s.trackColumns) ^ col);
        s.markDirty();
        if (!ref.expired()) {
            requestLayout();
            invalidate();
        }
    };
    std::vector<ui::MenuItem> items;
    auto item = [&](const wchar_t* label, uint32_t col, bool available) {
        if (!available) return;
        ui::MenuItem it{tr(label), "", L"", [toggle, col] { toggle(col); }};
        it.checked = (user & col) != 0;
        items.push_back(std::move(it));
    };
    item(L"Albüm", trackcols::Album, opts_.showAlbum);
    item(L"Eklenme tarihi", trackcols::Added, opts_.showAdded);
    item(L"Dinlenme sayısı", trackcols::Plays, hasPlays_);
    if (items.empty()) return;
    items.push_back(ui::MenuItem::sep());
    items.push_back({tr(L"Varsayılan sütunlar"), "refresh", L"", [ref = life_.ref(), this] {
                         auto& s = Settings::get();
                         s.trackColumns.clear();
                         s.markDirty();
                         if (!ref.expired()) {
                             requestLayout();
                             invalidate();
                         }
                     }});
    ui::Menu::open(ctx().window, windowPos, std::move(items));
}

// ---- Rows -------------------------------------------------------------------------------------------------------------

void TrackTable::paintSkeletonRows(Canvas& c, ui::Widget* owner, const Rect& area, int rows, int seed, bool art) {
    const RowMetrics m = rowMetrics();
    const Layout cl = trackcols::layout({area.x, area.w, trackcols::Album, m.compact});
    const Rect vis = c.clip();
    for (int k = 0; k < rows; ++k) {
        const Rect rr{area.x, area.y + k * m.h, area.w, m.h};
        if (!rr.intersects(vis)) continue;
        const int v = (seed + k) * 7 % 5;   // bar lengths vary, so the rows read like text
        ui::paintSkeleton(c, owner, {cl.num, rr.cy() - 5, 18, 10}, gfx::radii().badge);
        float x = cl.title;
        if (art) {
            ui::paintSkeleton(c, owner, {x, rr.cy() - m.art * 0.5f, m.art, m.art}, gfx::radii().art);
            x += m.art + m.artGap;
        }
        const float tw = std::min(cl.titleW - (x - cl.title), 150.f + v * 22.f);
        ui::paintSkeleton(c, owner, {x, rr.y + m.titleY + m.titleH * 0.5f - 5, tw, 10}, gfx::radii().badge);
        ui::paintSkeleton(c, owner, {x, rr.y + m.artistY + m.artistH * 0.5f - 4, std::min(tw, 80.f + v * 14.f), 8},
                          gfx::radii().badge);
        if (cl.albumW > 0)
            ui::paintSkeleton(c, owner, {cl.album, rr.cy() - 4, std::min(cl.albumW, 90.f + (4 - v) * 16.f), 8},
                              gfx::radii().badge);
        ui::paintSkeleton(c, owner, {cl.dur + cl.durW - 34, rr.cy() - 4, 34, 8}, gfx::radii().badge);
    }
}

// Selected rows: each run of consecutive ones is one block (accent.tint12, rounded only at its ends) with one
// continuous 2 DIP accent bar on the left (the spec's inset bar), so a multi-selection reads as one shape.
void TrackTable::paintSelection(Canvas& c, int first, int last) {
    if (selected_.empty() || first > last) return;
    const auto& acc = accent();
    for (const auto& run : trackcols::selectionRuns(first, last, [this](int i) { return selected_.contains(view_[i]); })) {
        const Rect a = rowRect(run.first), b = rowRect(run.last);
        const Rect block{a.x, a.y, a.w, b.bottom() - a.y};
        c.fillRounded(block, gfx::radii().surface, acc.tint12);
        c.fillRect({block.x, block.y, 2, block.h}, acc.base);
    }
}

void TrackTable::paintRow(Canvas& c, int i, const Rect& r, const Layout& cl) {
    const auto& col = colors();
    const auto& acc = accent();
    const int ti = view_[i];
    const auto& t = tracks_[ti];
    const bool selected = selected_.contains(ti);
    const auto* cur = ctx().player ? ctx().player->current() : nullptr;
    const bool isCurrent = cur && !t.id.empty() && cur->id == t.id;
    const bool playing = isCurrent && ctx().player->isPlaying();
    const bool hot = i == hover_;
    const float hv = hoverFade_.of(i);   // the hover look fades in / out (ui::HoverFade)
    // The row actions show on the hovered row and on the keyboard cursor's.
    const bool cursorRow = keyboardFocused() && i == std::clamp(anchor_, 0, static_cast<int>(view_.size()) - 1);
    const float act = std::max(hv, cursorRow ? 1.f : 0.f);
    const float slide = (1.f - ui::ease(ui::Ease::Decelerate, act)) * 6.f;   // the actions slide in 6 DIP from the right
    auto& tx = rowText(i);
    const RowMetrics m = rowMetrics();
    // Unplayable and blocked (kara liste) rows are dimmed; a blocked row still plays when picked directly. Rows of this
    // table being dragged to a new place are dimmed while they travel.
    float alpha = t.playable && !tx.blocked ? 1.f : 0.38f;
    if (dragging_ && !reorderRows_.empty() && selected && dropAt_ >= 0) alpha *= 0.45f;

    // The playing row: a faint accent tint, so it is found at a glance while scrolling (selection runs are painted
    // before the rows; a gradient this faint bands on paper).
    if (isCurrent && !selected) c.fillRounded(r, gfx::radii().surface, acc.tint06);
    if (hv > 0.001f) c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(hv));

    // Index cell: the number; the playing song's bars (moving while it plays, frozen while paused); on hover a cross-fade
    // to the play / pause glyph (120 ms). Several rows selected: the selected ones show a checked box instead.
    const Rect numR{cl.num, r.y, cl.numW, r.h};
    if (multiSelect() && selected) {
        const Rect box{numR.x, numR.cy() - 7, 14, 14};
        c.fillRounded(box, gfx::radii().badge, acc.base);
        c.icon("check", box.center(10, 10), acc.onAccent);
    } else {
        const float glyph = t.playable ? hv : 0.f;
        if (glyph < 0.999f) {
            if (isCurrent) drawPlayingBars(c, this, {numR.x, numR.cy() - 7, 14, 14}, acc.base.mulAlpha(1 - glyph));
            else c.text(tx.num, {numR.x, r.y, numR.w, r.h}, col.fgTertiary.mulAlpha(alpha * (1 - glyph)), gfx::VAlign::Center);
        }
        if (glyph > 0.001f) c.icon(playing ? "pause" : "play", {numR.x, numR.cy() - 6, 12, 12}, col.fgPrimary.mulAlpha(glyph));
    }

    // Title block: art, the title with its badges, the artists (a title alone is centered on the row).
    float x0 = cl.title;
    if (opts_.showArt) {
        const Rect art{cl.title, r.cy() - m.art * 0.5f, m.art, m.art};
        drawArtwork(c, t.album.images, art, gfx::radii().art, Placeholder::Album, false, Priority::High);
        if (alpha < 1) c.fillRect(art, col.bgBase.withAlpha(std::clamp(1 - alpha * 1.05f, 0.f, 1.f)));
        x0 += m.art + m.artGap;
    }
    const float blockR = cl.title + cl.titleW;
    const bool local = opts_.markLocal && t.id.rfind("local:", 0) == 0;
    const std::wstring addedBy = together::addedBy(t);
    float badgesW = 0;
    if (t.explicitContent) badgesW += 14 + badge::kGap;
    if (tx.downloaded) badgesW += badge::kMark + badge::kGap;
    if (t.recommended) badgesW += smartshuffle::badgeWidth() + badge::kGap;
    if (!addedBy.empty()) badgesW += together::addedBadgeWidth(addedBy) + badge::kGap;
    if (tx.blocked) badgesW += badge::width(badge::blockedLabel()) + badge::kGap;
    if (local) badgesW += badge::width(badge::localLabel()) + badge::kGap;
    const float roomW = std::max(0.f, blockR - x0);
    // Badges keep their room while the title has 120 DIP; below that they go before the title is cut further.
    const bool badges = badgesW > 0 && roomW - badgesW - 8 >= std::min(120.f, tx.titleW);
    const float titleTextW = badges ? roomW - badgesW - 2 : roomW;
    const bool alone = tx.artist.empty() && t.playable;
    const float titleY = alone ? r.cy() - m.titleH * 0.5f : r.y + m.titleY;
    c.text(tx.title, {x0, titleY, titleTextW, m.titleH}, (isCurrent ? acc.base : col.fgPrimary).mulAlpha(alpha),
           gfx::VAlign::Center);
    if (badges) {
        const float cy = titleY + m.titleH * 0.5f;
        float bx = x0 + std::min(titleTextW, tx.titleW) + 8;
        // Full strength on dimmed rows: the marks explain why they are dimmed.
        if (t.explicitContent) bx += badge::drawExplicit(c, bx, cy) + badge::kGap;
        if (tx.downloaded) bx += badge::drawDownloaded(c, bx, cy) + badge::kGap;
        if (t.recommended) bx += smartshuffle::drawBadge(c, bx, cy) + badge::kGap;
        if (!addedBy.empty()) bx += together::drawAddedBadge(c, bx, cy, addedBy) + badge::kGap;
        if (tx.blocked) bx += badge::draw(c, bx, cy, badge::blockedLabel(), badge::Tone::Neutral) + badge::kGap;
        if (local) badge::draw(c, bx, cy, badge::localLabel(), badge::Tone::Neutral);
    }
    if (!alone) {
        const bool artistHot = hot && hoverHit_ == Hit::Artist;
        const Color artistCol = (artistHot ? col.fgPrimary : col.fgSecondary).mulAlpha(alpha);
        const Rect ar{x0, r.y + m.artistY, roomW, m.artistH};
        c.text(tx.artist, ar, artistCol, gfx::VAlign::Center);
        const float aw = std::min(roomW, tx.artistW);
        if (artistHot) c.hline(x0, x0 + aw, std::round(ar.y + ar.h * 0.5f + m.artistH * 0.36f), artistCol);
        if (!t.playable && roomW - aw > 110)
            badge::draw(c, x0 + aw + 8, ar.cy(), badge::unavailableLabel(), badge::Tone::Neutral);
    }
    if (cl.albumW > 0) c.text(tx.album, {cl.album, r.y, cl.albumW, r.h}, col.fgSecondary.mulAlpha(alpha), gfx::VAlign::Center);
    if (cl.addedW > 0) c.text(tx.added, {cl.added, r.y, cl.addedW, r.h}, col.fgTertiary.mulAlpha(alpha), gfx::VAlign::Center);
    if (cl.playsW > 0) c.text(tx.plays, {cl.plays, r.y, cl.playsW, r.h}, col.fgTertiary.mulAlpha(alpha), gfx::VAlign::Center);

    // Actions: [add to queue][heart] 03:42 [more]. The heart stays while liked; the others fade and slide in on hover
    // (into room that is always reserved: nothing moves). A "Geliştir" recommendation: "+" (add it to the list) in the
    // heart's place, "×" (hide it) in the queue's.
    auto actionInk = [&](Hit h) { return (hot && hoverHit_ == h ? col.fgPrimary : col.fgSecondary); };
    if (t.recommended && onAddRecommended) {
        c.icon("plus", slotIcon(cl.heart, r), hot && hoverHit_ == Hit::RecAdd ? acc.base : col.fgPrimary);
        if (act > 0.001f && onHideRecommended && cl.queueW > 0)
            c.icon("close", slotIcon(cl.queue, r, slide).inset(1), actionInk(Hit::RecHide).mulAlpha(act));
    } else {
        const bool liked = ctx().library.isLiked(t.id);
        const bool popping = !popId_.empty() && popId_ == t.id && pop_.running();
        const float heartA = liked || popping ? 1.f : ctx().library.canLike(t.id) ? act : 0.f;
        if (heartA > 0.001f) {
            const Rect hr = slotIcon(cl.heart, r);
            const Color outline = actionInk(Hit::Heart).mulAlpha(heartA);
            if (popping) pop_.draw(c, hr, liked, "heart", "heart-filled", outline, acc.base);
            else c.icon(liked ? "heart-filled" : "heart", hr, liked ? acc.base : outline);
        }
        if (act > 0.001f && cl.queueW > 0 && t.playable)
            c.icon("queue", slotIcon(cl.queue, r, slide), actionInk(Hit::Queue).mulAlpha(act));
    }
    c.text(tx.dur, {cl.dur, r.y, cl.durW, r.h}, col.fgSecondary.mulAlpha(alpha), gfx::VAlign::Center);
    if (act > 0.001f) c.icon("more", slotIcon(cl.more, r, slide), actionInk(Hit::More).mulAlpha(act));
}

void TrackTable::paint(Canvas& c) {
    const Rect r = rect();
    if (const uint64_t rev = blacklist::revision(); rev != blockRev_) {   // blocked / unblocked: re-evaluate the rows
        blockRev_ = rev;
        texts_.clear();
    }
    // The playing song liked / unliked elsewhere (the player bar's heart, Ctrl+L, a menu): its row's heart answers too.
    if (const auto* cur = ctx().player ? ctx().player->current() : nullptr) {
        const bool liked = ctx().library.isLiked(cur->id);
        if (cur->id == curId_ && liked != curLiked_ && !(popId_ == cur->id && pop_.running())) {
            popId_ = cur->id;
            pop_.start(liked);
        }
        curId_ = cur->id;
        curLiked_ = liked;
    }
    const Layout cl = columns();
    // Only rows intersecting the visible clip are painted (virtualization).
    const Rect vis = c.clip();
    const float top = r.y + headerHeight();
    const RowMetrics m = rowMetrics();
    const int first = std::max(0, static_cast<int>((vis.y - top) / m.h));
    const int last = std::min(static_cast<int>(view_.size()) - 1, static_cast<int>((vis.bottom() - top) / m.h));
    paintSelection(c, first, last);
    for (int i = first; i <= last; ++i) paintRow(c, i, rowRect(i), cl);
    if (dropAt_ >= 0) {   // own rows dragged over the list: they go in front of this row (the line glides there)
        const float y = std::round(dropY_.value());
        c.fillRect({r.x + 9, y - 1, r.w - 9, 2}, accent().base);
        c.strokeCircle({r.x + 4.5f, y}, 3.5f, accent().base, 1.5f);
    }
    // Skeleton rows while the next page loads.
    if (loadingRows_ > 0)
        paintSkeletonRows(c, this, {r.x, rowRect(static_cast<int>(view_.size())).y, r.w, loadingRows_ * m.h}, loadingRows_,
                          static_cast<int>(view_.size()), opts_.showArt);
    // The column header: in place, or stuck to the top of the visible area once the table scrolls under it (a docked
    // header is the collapsing bar's to paint).
    stickyY_ = -1;
    if (opts_.showHeader && (!tracks_.empty() || loadingRows_ > 0)) {   // an empty list shows its empty state alone
        float hy = r.y;
        if (!headerDocked_) {
            const ui::ScrollView* sv = scrollViewAbove(this);
            const float viewTop = vis.y + (sv ? sv->topInset : 0.f);
            if (viewTop > r.y && !view_.empty()) {
                hy = std::min(viewTop, r.bottom() - 8 - headerHeight());
                stickyY_ = hy;
                c.fillSurface({r.x, hy, r.w, kHeaderH + 1});
            }
        }
        if (stickyY_ >= 0 || r.y + kHeaderH >= vis.y) paintHeader(c, {r.x, hy, r.w, kHeaderH});
    }
    // Ask for more when the bottom 1.5 screens become visible.
    if (onNearEnd && !nearEndSignaled_ && last >= static_cast<int>(view_.size()) - 30 && !view_.empty()) {
        nearEndSignaled_ = true;
        onNearEnd();
    }
}

TrackTable::Hit TrackTable::hitAt(int i, gfx::Point p) {
    if (i < 0) return Hit::None;
    const Layout cl = columns();
    const Rect rr = rowRect(i);
    const auto& t = tracks_[view_[i]];
    if (p.x >= cl.num && p.x < cl.num + cl.numW) return Hit::Number;
    const bool rec = t.recommended && onAddRecommended;
    if (slotRect(cl.more, rr).contains(p)) return Hit::More;
    if (slotRect(cl.heart, rr).contains(p)) {
        if (rec) return Hit::RecAdd;
        if (ctx().library.canLike(t.id)) return Hit::Heart;
    }
    if (cl.queueW > 0 && slotRect(cl.queue, rr).contains(p)) {
        if (rec && onHideRecommended) return Hit::RecHide;
        if (!rec && t.playable) return Hit::Queue;
    }
    // The artists' text (it opens the first artist).
    const RowMetrics m = rowMetrics();
    const float x0 = cl.title + (opts_.showArt ? m.art + m.artGap : 0);
    auto& tx = rowText(i);
    if (!tx.artist.empty()) {
        const float w = std::min(cl.title + cl.titleW - x0, tx.artistW);
        if (Rect{x0, rr.y + m.artistY, w, m.artistH}.contains(p)) return Hit::Artist;
    }
    return Hit::None;
}

void TrackTable::setHover(int row, Hit hit) {
    if (row == hover_ && hit == hoverHit_) return;
    if (row != hover_) hoverFade_.set(row);
    hover_ = row;
    hoverHit_ = hit;
    invalidate();
}

void TrackTable::onMouseMove(const ui::MouseEvent& e) {
    // A pressed row that travels past the threshold becomes a drag of its songs.
    if (pressRow_ >= 0 && pressed() && !dragging_ &&
        std::hypot(e.windowPos.x - pressPos_.x, e.windowPos.y - pressPos_.y) > dragdrop::kThreshold) {
        auto tracks = dragTracks();
        if (!tracks.empty()) {
            dragging_ = true;
            collapseOnUp_ = -1;
            reorderRows_ = canReorder() ? dragRows() : std::vector<int>{};
            dragdrop::begin(std::move(tracks), e.windowPos);
        }
    }
    if (dragging_) dragdrop::move(e.windowPos);
    // The header (in place or stuck to the top): its sortable labels light up.
    const bool onHeader = opts_.showHeader && ((e.pos.y >= rect().y && e.pos.y < rect().y + kHeaderH) ||
                                              (stickyY_ >= 0 && e.pos.y >= stickyY_ && e.pos.y < stickyY_ + kHeaderH));
    if (onHeader && !dragging_) {
        headerHover(e.windowPos);
        setHover(-1, Hit::None);
        return;
    }
    headerLeave();
    const int i = dragging_ ? -1 : rowAt(e.pos.y);
    setHover(i, hitAt(i, e.pos));
}

void TrackTable::onMouseLeave() {
    headerLeave();
    setHover(-1, Hit::None);
}

LPCWSTR TrackTable::cursor() const {
    if (headerHot_ >= 0) return IDC_HAND;
    switch (hoverHit_) {
    case Hit::Artist:
    case Hit::Heart:
    case Hit::Queue:
    case Hit::More:
    case Hit::RecAdd:
    case Hit::RecHide: return IDC_HAND;
    default: return IDC_ARROW;
    }
}

bool TrackTable::onMouseDown(const ui::MouseEvent& e) {
    const bool onHeader = opts_.showHeader && ((e.pos.y >= rect().y && e.pos.y < rect().y + kHeaderH) ||
                                              (stickyY_ >= 0 && e.pos.y >= stickyY_ && e.pos.y < stickyY_ + kHeaderH));
    if (onHeader) {
        if (e.button == ui::MouseButton::Left) headerClick(e.pos.x);
        else if (e.button == ui::MouseButton::Right) showColumnsMenu(e.windowPos);
        return e.button == ui::MouseButton::Left;
    }
    const int i = rowAt(e.pos.y);
    if (i < 0) return false;
    const int ti = view_[i];
    const auto& t = tracks_[ti];
    const Hit hit = hitAt(i, e.pos);
    if (e.button == ui::MouseButton::Right) {
        if (!selected_.contains(ti)) {
            selected_ = {ti};
            anchor_ = rangeFrom_ = i;
        }
        showTrackMenu(selectedTracks(), e.windowPos, playlistId_, recommendKey_, menuRemove());
        invalidate();
        return false;
    }
    if (e.button != ui::MouseButton::Left) return false;
    switch (hit) {
    case Hit::RecAdd: {
        const auto add = onAddRecommended;   // the page may rebuild the table
        add(t);
        return true;
    }
    case Hit::RecHide: {
        const auto hide = onHideRecommended;
        hide(t);
        return true;
    }
    case Hit::Heart: {
        const std::string id = t.id;   // the library's notify may rebuild the rows
        ctx().library.toggleLiked(t);
        popId_ = id;
        pop_.start(ctx().library.isLiked(id));
        invalidate();
        return true;
    }
    case Hit::Queue: {
        auto picked = selected_.contains(ti) && multiSelect() ? selectedTracks() : std::vector<catalog::Track>{t};
        for (auto& x : picked) x.recommended = false;
        ctx().player->enqueue(picked);
        toast(picked.size() > 1 ? i18n::plural(L"{} şarkı sıraya eklendi", static_cast<int64_t>(picked.size()))
                                : std::wstring(tr(L"Sıraya eklendi")));
        return true;
    }
    case Hit::More: {
        if (!selected_.contains(ti)) {
            selected_ = {ti};
            anchor_ = rangeFrom_ = i;
        }
        const Rect slot = toWindow(slotRect(columns().more, rowRect(i)));
        showTrackMenu(selectedTracks(), {slot.x, slot.bottom() + 2}, playlistId_, recommendKey_, menuRemove());
        invalidate();
        return true;
    }
    case Hit::Artist:
        if (!t.artists.empty() && !t.artists[0].id.empty()) {
            ctx().router->navigate({RouteKind::Artist, t.artists[0].id});
            return true;
        }
        break;
    case Hit::Number:
        // Several rows selected: a selected row's box takes it out of the selection (a ctrl-click).
        if (multiSelect() && selected_.contains(ti)) {
            selected_.erase(ti);
            anchor_ = rangeFrom_ = i;
            invalidate();
            return true;
        }
        break;
    default: break;
    }
    if (e.clicks == 2 || hit == Hit::Number) {
        const auto* cur = ctx().player->current();
        if (hit == Hit::Number && cur && cur->id == t.id) ctx().player->togglePause();
        else if (onPlay && t.playable) onPlay(i);
        return true;
    }
    collapseOnUp_ = -1;
    if (e.ctrl) {
        if (selected_.contains(ti)) selected_.erase(ti);
        else selected_.insert(ti);
        anchor_ = rangeFrom_ = i;
    } else if (e.shift && (rangeFrom_ >= 0 || anchor_ >= 0)) {
        const int from = std::clamp(rangeFrom_ >= 0 ? rangeFrom_ : anchor_, 0, static_cast<int>(view_.size()) - 1);
        selected_.clear();
        for (int k = std::min(from, i); k <= std::max(from, i); ++k) selected_.insert(view_[k]);
        anchor_ = i;
    } else if (selected_.contains(ti) && selected_.size() > 1) {
        collapseOnUp_ = ti;   // keep the selection: this press may drag all of it
        anchor_ = rangeFrom_ = i;
    } else {
        selected_ = {ti};
        anchor_ = rangeFrom_ = i;
    }
    pressRow_ = i;
    pressPos_ = e.windowPos;
    invalidate();
    return true;
}

void TrackTable::onMouseUp(const ui::MouseEvent& e) {
    if (dragging_) {
        dragging_ = false;
        dragdrop::finish(e.windowPos);
    } else if (collapseOnUp_ >= 0) {
        selected_ = {collapseOnUp_};
        invalidate();
    }
    reorderRows_.clear();
    dropAt_ = -1;
    collapseOnUp_ = -1;
    pressRow_ = -1;
}

// ---- Reordering (the table as a drop target for its own rows) ------------------------------------------------------

bool TrackTable::dragOver(const DragPayload& payload, gfx::Point windowPos) {
    if (reorderRows_.empty() || payload.fromExplorer() || !canReorder()) return false;
    autoScroll(windowPos);
    const int at = dropIndexAt(windowPos);
    if (at != dropAt_) {
        const float y = rowRect(at).y;
        if (dropAt_ < 0) dropY_.snap(y);
        else dropY_.to(y, ui::motion::microMs(ui::motion::fast), ui::Ease::Decelerate);
        dropAt_ = at;
        invalidate();
    }
    return true;
}

void TrackTable::dragLeave() {
    // reorderRows_ stays until the release: drop() comes right after this.
    lastScrollTick_ = 0;
    if (dropAt_ >= 0) {
        dropAt_ = -1;
        invalidate();
    }
}

void TrackTable::drop(const DragPayload&, gfx::Point windowPos) {
    if (reorderRows_.empty() || !canReorder()) return;
    const int at = dropIndexAt(windowPos);
    // Custom order, unfiltered: display index == tracks_ index.
    const int before = at < static_cast<int>(view_.size()) ? view_[at] : static_cast<int>(tracks_.size());
    std::vector<int> rows = std::move(reorderRows_);
    reorderRows_.clear();
    dropAt_ = -1;
    // After the event: the page reorders (or rebuilds) the table, and the release is still being handled here.
    Dispatcher::post([ref = life_.ref(), this, rows = std::move(rows), before] {
        if (!ref.expired() && onReorder) onReorder(rows, before);
    });
}

int TrackTable::dropIndexAt(gfx::Point windowPos) const {
    const gfx::Point p = fromWindow(windowPos);
    const float top = rect().y + headerHeight();
    const int i = static_cast<int>(std::floor((p.y - top) / rowMetrics().h + 0.5f));
    return std::clamp(i, 0, static_cast<int>(view_.size()));
}

void TrackTable::autoScroll(gfx::Point windowPos) {
    const double now = ui::frame::realNow(), last = lastScrollTick_;
    lastScrollTick_ = now;
    if (last <= 0) return;
    ui::ScrollView* sv = scrollViewAbove(this);
    if (!sv) return;
    // Near the visible top (below a sticky header) / bottom edge: faster the closer the pointer is to the edge.
    const Rect vr = sv->toWindow(sv->rect());
    const float top = vr.y + sv->topInset + (headerDocked_ ? 0.f : kHeaderH), bottom = vr.bottom(), edge = 48,
                y = windowPos.y;
    float v = 0;
    if (y >= top && y < top + edge) v = -(1 - (y - top) / edge);
    else if (y > bottom - edge && y <= bottom) v = 1 - (bottom - y) / edge;
    if (v == 0) return;
    const float dt = static_cast<float>(std::min(now - last, 100.0)) / 1000.f;
    sv->scrollTo(std::clamp(sv->scrollY() + v * 900.f * dt, 0.f, sv->maxScroll()), false);
}

std::function<void(const std::vector<catalog::Track>&)> TrackTable::menuRemove() {
    if (!onRemove) return {};
    return [ref = life_.ref(), fn = onRemove](const std::vector<catalog::Track>& rows) {
        if (!ref.expired()) fn(rows);
    };
}

std::vector<catalog::Track> TrackTable::dragTracks() const {
    std::vector<catalog::Track> out;
    if (pressRow_ < 0 || pressRow_ >= static_cast<int>(view_.size())) return out;
    const int pressed = view_[pressRow_];
    if (!selected_.contains(pressed)) {   // a ctrl-click just took it out of the selection: drag it alone
        if (!tracks_[pressed].id.empty()) out.push_back(tracks_[pressed]);
    } else {
        for (int k : view_)   // display order
            if (selected_.contains(k) && !tracks_[k].id.empty()) out.push_back(tracks_[k]);
    }
    for (auto& t : out) t.recommended = false;   // dropped somewhere, a recommendation becomes the user's song
    return out;
}

std::vector<int> TrackTable::dragRows() const {
    std::vector<int> out;
    if (pressRow_ < 0 || pressRow_ >= static_cast<int>(view_.size())) return out;
    const int pressed = view_[pressRow_];
    if (!selected_.contains(pressed)) return {pressed};
    for (int k : view_)
        if (selected_.contains(k)) out.push_back(k);
    return out;
}

// ---- Song preview (app/Preview) ----------------------------------------------------------------------------------------

std::optional<PreviewHit> TrackTable::previewAt(gfx::Point windowPos, bool keyboard) {
    if (view_.empty()) return std::nullopt;
    int i = -1;
    if (keyboard) {
        i = std::clamp(anchor_, 0, static_cast<int>(view_.size()) - 1);
    } else {
        const gfx::Point p = fromWindow(windowPos);
        if (opts_.showHeader && p.y < rect().y + kHeaderH) return std::nullopt;
        i = rowAt(p.y);
        if (i < 0 || hitAt(i, p) != Hit::None) return std::nullopt;   // the number, the artists and the actions act
    }
    const auto& t = tracks_[view_[i]];
    if (!t.playable) return std::nullopt;
    return PreviewHit{preview::songTarget(t), toWindow(rowRect(i))};
}

std::optional<PreviewHit> MediaCard::previewAt(gfx::Point windowPos, bool keyboard) {
    if (!previewTarget) return std::nullopt;
    if (!keyboard) {
        const gfx::Point p = fromWindow(windowPos);
        if (!artRect().contains(p) || (onPlay && playRect().contains(p))) return std::nullopt;
    }
    return PreviewHit{*previewTarget, toWindow(artRect())};
}

std::vector<catalog::Track> TrackTable::selectedOwnRows() const {
    std::vector<catalog::Track> out;
    for (int k : view_)
        if (selected_.contains(k) && !tracks_[k].recommended) out.push_back(tracks_[k]);
    return out;
}

std::vector<catalog::Track> TrackTable::selectedTracks() const {
    std::vector<catalog::Track> out;
    for (int k : view_)
        if (selected_.contains(k)) out.push_back(tracks_[k]);
    return out;
}

// Keyboard cursor = anchor_ (display index). The Window draws the focus ring around it and scrolls it into view
// after every arrow key; before the first move (Tab into the table) the cursor is row 0.
Rect TrackTable::focusRect() const {
    if (view_.empty()) return rect();
    return rowRect(std::clamp(anchor_, 0, static_cast<int>(view_.size()) - 1));
}

bool TrackTable::onKeyDown(const ui::KeyEvent& e) {
    if (dragging_ && e.vk == VK_ESCAPE) {
        dragging_ = false;
        pressRow_ = -1;
        reorderRows_.clear();
        dragdrop::cancel();
        return true;
    }
    if (view_.empty()) return false;
    if (e.vk == VK_DELETE && !e.ctrl && !e.alt) {   // remove the selected songs from this list
        if (!onRemove || dragging_) return false;
        auto rows = selectedOwnRows();
        if (rows.empty() || e.repeat) return true;
        Dispatcher::post([ref = life_.ref(), this, rows = std::move(rows)] {
            if (!ref.expired() && onRemove) onRemove(rows);
        });
        return true;
    }
    const int n = static_cast<int>(view_.size());
    int cur = std::min(anchor_, n - 1);   // the view may have shrunk (filter) since the anchor was set
    switch (e.vk) {
    case VK_DOWN: cur = std::min(n - 1, cur + 1); break;
    case VK_UP: cur = std::max(0, cur - 1); break;
    case VK_NEXT: cur = std::min(n - 1, std::max(cur, 0) + 10); break;   // PageDown
    case VK_PRIOR: cur = std::max(0, cur - 10); break;                   // PageUp
    case VK_HOME: cur = 0; break;
    case VK_END: cur = n - 1; break;
    case VK_ESCAPE:   // several rows picked: back to the cursor's row alone
        if (!multiSelect()) return false;
        cur = std::max(cur, 0);
        selected_ = {view_[cur]};
        rangeFrom_ = cur;
        invalidate();
        return true;
    case VK_RETURN: {
        const int i = std::max(cur, 0);
        if (onPlay && tracks_[view_[i]].playable) onPlay(i);
        return true;
    }
    case VK_APPS:
    case VK_F10: {
        if (e.vk == VK_F10 && !e.shift) return false;
        const int i = std::max(cur, 0);
        if (!selected_.contains(view_[i])) {
            selected_ = {view_[i]};
            anchor_ = rangeFrom_ = i;
        }
        const Rect rr = toWindow(rowRect(i));
        showTrackMenu(selectedTracks(), {rr.x + 48, rr.bottom()}, playlistId_, recommendKey_, menuRemove());
        invalidate();
        return true;
    }
    case 'A':
        if (e.ctrl) {
            for (int i : view_) selected_.insert(i);
            invalidate();
            return true;
        }
        return false;
    default: return false;
    }
    if (e.shift) {   // extend the selection from where the range started
        const int from = std::clamp(rangeFrom_ >= 0 ? rangeFrom_ : std::max(anchor_, 0), 0, n - 1);
        selected_.clear();
        for (int k = std::min(from, cur); k <= std::max(from, cur); ++k) selected_.insert(view_[k]);
        rangeFrom_ = from;
    } else {
        selected_ = {view_[cur]};
        rangeFrom_ = cur;
    }
    anchor_ = cur;
    invalidate();
    return true;
}

} // namespace st::app
