#include "app/LibraryView.h"

#include "app/Accounts.h"
#include "app/DownloadSync.h"
#include "core/Dispatcher.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "gfx/Theme.h"
#include "ui/Layout.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace st::app {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;

namespace {

constexpr float kGap = gfx::metrics::cardGap;   // the card grid's gap (addCardRow)
constexpr size_t kMaxTexts = 240;               // cached cells' text layouts (a few screens)

ui::ScrollView* scrollViewOf(const ui::Widget* w) {
    for (ui::Widget* p = w->parent(); p; p = p->parent())
        if (auto* sv = dynamic_cast<ui::ScrollView*>(p)) return sv;
    return nullptr;
}

} // namespace

LibraryView::LibraryView(Mode mode, float minCell) : mode_(mode), minCell_(minCell) { focusable = true; }

LibraryView::~LibraryView() { dragdrop::forgetTarget(this); }

void LibraryView::setCells(std::shared_ptr<const std::vector<LibraryCell>> cells, std::vector<uint32_t> order) {
    // The keyboard cursor and the hover stay on the same cells (a re-sort, a filter, a library update).
    const int n = static_cast<int>(order_.size());
    const std::string cursorKey = cells_ && cursor_ >= 0 && cursor_ < n ? cell(cursor_).key : std::string();
    const std::string hotKey = cells_ && hot_ >= 0 && hot_ < n ? cell(hot_).key : std::string();
    cells_ = std::move(cells);
    order_ = std::move(order);
    if (!cells_) order_.clear();
    auto find = [this](const std::string& key, int fallback) {
        if (!key.empty())
            for (int d = 0; d < static_cast<int>(order_.size()); ++d)
                if (cell(d).key == key) return d;
        return fallback;
    };
    cursor_ = find(cursorKey, 0);
    // Hover only where the same cell is still under the pointer (a re-sort may have moved it away).
    if (hot_ < 0 || find(hotKey, -1) != hot_) {
        hot_ = -1;
        playHot_ = false;
    }
    prevHot_ = pressed_ = dropHot_ = -1;
    texts_.clear();
    requestLayout();
    invalidate();
}

void LibraryView::setMode(Mode mode) {
    if (mode == mode_) return;
    mode_ = mode;
    hot_ = prevHot_ = pressed_ = dropHot_ = -1;
    texts_.clear();
    requestLayout();
    invalidate();
}

// ---- Geometry ----------------------------------------------------------------------------------------------------------

int LibraryView::columns(float width) const {
    if (mode_ == Mode::List) return 1;
    return std::max(1, static_cast<int>((width + kGap) / (minCell_ + kGap)));
}

float LibraryView::cellWidth(float width) const {
    const int cols = columns(width);
    return mode_ == Mode::List ? width : (width - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
}

// A grid cell is a MediaCard: the square artwork, then the title and the subtitle.
float LibraryView::cellHeight(float width) const { return mode_ == Mode::List ? kRowH : cellWidth(width) + 10 + 18 + 4 + 16; }

float LibraryView::preferredHeight(float width) {
    const int n = static_cast<int>(order_.size());
    if (n == 0) return 0;
    if (mode_ == Mode::List) return n * kRowH;
    const int cols = columns(width);
    const int rows = (n + cols - 1) / cols;
    return rows * cellHeight(width) + static_cast<float>(rows - 1) * kGap;
}

Rect LibraryView::cellRect(int d) const {
    const Rect r = rect();
    if (mode_ == Mode::List) return {r.x, r.y + d * kRowH, r.w, kRowH};
    const int cols = columns(r.w);
    const float cw = cellWidth(r.w), ch = cellHeight(r.w);
    const int row = d / cols, col = d % cols;
    return {r.x + col * (cw + kGap), r.y + row * (ch + kGap), cw, ch};
}

Rect LibraryView::artRect(int d) const {
    const Rect c = cellRect(d);
    if (mode_ == Mode::List) return {c.x + 8, c.cy() - 24, 48, 48};
    return {c.x, c.y, c.w, c.w};
}

Rect LibraryView::playRect(int d) const {
    const Rect a = artRect(d);
    if (mode_ == Mode::List) return a.center(32, 32);
    return {a.right() - 12 - 48, a.bottom() - 12 - 48, 48, 48};   // MediaCard's
}

int LibraryView::cellAt(gfx::Point p) const {
    const Rect r = rect();
    const int n = static_cast<int>(order_.size());
    if (n == 0 || !r.contains(p)) return -1;
    if (mode_ == Mode::List) {
        const int d = static_cast<int>((p.y - r.y) / kRowH);
        return d < n ? d : -1;
    }
    const int cols = columns(r.w);
    const float cw = cellWidth(r.w), ch = cellHeight(r.w);
    const float lx = p.x - r.x, ly = p.y - r.y;
    const int col = static_cast<int>(lx / (cw + kGap)), row = static_cast<int>(ly / (ch + kGap));
    if (col >= cols || lx - col * (cw + kGap) > cw || ly - row * (ch + kGap) > ch) return -1;   // in a gap
    const int d = row * cols + col;
    return d < n ? d : -1;
}

Rect LibraryView::focusRect() const {
    if (order_.empty()) return rect();
    const int d = std::clamp(cursor_, 0, static_cast<int>(order_.size()) - 1);
    return mode_ == Mode::Grid && cell(d).circle ? artRect(d) : cellRect(d);   // round cards: the artwork
}

ui::FocusShape LibraryView::focusShape() const {
    if (order_.empty() || mode_ != Mode::Grid) return ui::FocusShape::Rect;
    return cell(std::clamp(cursor_, 0, static_cast<int>(order_.size()) - 1)).circle ? ui::FocusShape::Circle
                                                                                    : ui::FocusShape::Rect;
}

// ---- Painting ----------------------------------------------------------------------------------------------------------

LibraryView::CellText& LibraryView::text(int d) {
    const uint32_t ci = order_[static_cast<size_t>(d)];
    if (auto it = texts_.find(ci); it != texts_.end()) return it->second;
    if (texts_.size() > kMaxTexts) texts_.clear();
    const LibraryCell& c = (*cells_)[ci];
    CellText t;
    if (mode_ == Mode::Grid) {
        gfx::TextOptions center;
        center.align = gfx::TextAlign::Center;
        t.title = c.circle ? gfx::Text(c.title, type::body, center) : gfx::Text(c.title, type::body.withSize(13));
        t.sub = c.circle ? gfx::Text(c.gridSubtitle, type::caption, center) : gfx::Text(c.gridSubtitle, type::caption);
    } else {
        t.title = gfx::Text(c.title, type::body);
        t.sub = gfx::Text(c.listSubtitle, type::caption);
        t.meta = gfx::Text({}, type::monoLabel);
    }
    return texts_.emplace(ci, std::move(t)).first->second;
}

float LibraryView::pointerHoverOf(int d) const {
    float h = 0;
    if (d == hot_) h = hotA_.value();
    if (d == prevHot_) h = std::max(h, prevA_.value());
    return h;
}

float LibraryView::hoverOf(int d) const {
    float h = pointerHoverOf(d);
    // Keyboard focus shows a square card's / a row's hover state (with its play button); round cards keep the ring.
    if (d == std::clamp(cursor_, 0, std::max(0, static_cast<int>(order_.size()) - 1)) && keyboardFocused() &&
        !(mode_ == Mode::Grid && cell(d).circle))
        h = 1;
    return h;
}

bool LibraryView::isCurrent(const LibraryCell& c) const {
    const auto* p = ctx().player;
    return p && !c.context.empty() && p->current() && p->context().uri == c.context;
}

void LibraryView::paintArt(Canvas& c, const LibraryCell& cell, const Rect& art, float radius) {
    if (pagetx::presence(&cell) <= 0.f) return;   // the page transition draws this cover
    if (cell.circle) {
        drawArtwork(c, cell.images, art, 0, Placeholder::Artist, true);
    } else if (cell.likedTile && cell.images.empty()) {   // Liked Songs as on its page: the accent tile and the heart
        c.fillRounded(art, radius, accent().base);
        const float s = std::max(20.f, std::round(art.w * 0.3f));
        c.icon("heart-filled", art.center(s, s), accent().onAccent);
    } else {
        drawArtwork(c, cell.images, art, radius, cell.placeholder);
    }
}

float LibraryView::paintMarks(Canvas& c, const LibraryCell& cell, float x, float cy) {
    const Color mark = accent().base;
    if (cell.pinned) {
        c.icon("pin", {x, cy - 6, 12, 12}, mark);
        x += 12 + 6;
    }
    if (!cell.syncId.empty() && sync::isSynced(cell.syncId)) {   // "Çevrimdışı kullanılabilir"
        c.icon("downloaded", {x, cy - 6, 12, 12}, mark);
        x += 12 + 6;
    }
    return x;
}

void LibraryView::paintPlay(Canvas& c, int d, const LibraryCell& cell, const Rect& pr, float h, float iconSize) {
    const auto& acc = accent();
    ui::card::Play play{h, d == hot_ ? playHotA_.value() : 0.f, d == hot_ && d == pressed_ ? playPressA_.value() : 0.f};
    play.pause = isCurrent(cell) && ctx().player->isPlaying();
    play.iconSize = iconSize;
    play.rise = mode_ == Mode::List ? 3.f : 8.f;   // the list row's button sits on a 48 DIP cover
    // On the accent heart tile an accent button would vanish: the inverse there.
    if (cell.likedTile && cell.images.empty()) ui::card::paintPlay(c, pr, play, acc.onAccent, acc.onAccent.mulAlpha(0.85f), acc.base);
    else ui::card::paintPlay(c, pr, play, acc.base, acc.hover, acc.onAccent);
}

void LibraryView::paintCard(Canvas& c, int d, const Rect& r) {
    const auto& col = colors();
    const auto& acc = accent();
    const LibraryCell& cl = cell(d);
    const float shown = pagetx::presence(&cl);   // 0 while the page transition draws this cover
    const float here = shown > 0.f ? 1.f : 0.f;
    const float h = hoverOf(d) * here, pointer = pointerHoverOf(d) * here;
    const Rect art{r.x, r.y, r.w, r.w};
    const float p = d == pressed_ && !pressPlay_ ? pressA_.value() : 0.f;
    // MediaCard's hover look (ui::card): the card rises over its shadow, the artwork's hairline brightens.
    c.pushTransform(D2D1::Matrix3x2F::Translation(0, ui::card::lift(pointer, p)));
    c.pushScale(ui::card::pressScale(p), {art.cx(), art.cy()});
    if (cl.circle) {
        ui::card::paintBack(c, art, pointer, true);
        paintArt(c, cl, art, 0);
        ui::card::paintEdge(c, art, h, true);
        if (d == dropHot_) c.strokeCircle({art.cx(), art.cy()}, art.w * 0.5f, acc.base, 2.f);
    } else {
        ui::card::paintBack(c, art, h * shown);
        paintArt(c, cl, art, gfx::radii().art);
        ui::card::paintEdge(c, art, h);
        if (d == dropHot_) {   // dragged songs would go here: the sidebar row's drop look
            c.fillRounded(art, gfx::radii().art, acc.tint12);
            c.strokeRounded(art, gfx::radii().art, acc.base);
        }
        if (cl.playable) paintPlay(c, d, cl, playRect(d), h, 16);
    }
    c.popTransform();
    auto& tx = text(d);
    const Rect tr{r.x, art.bottom() + 10, r.w, 18};
    c.text(tx.title, tr, isCurrent(cl) ? acc.base : col.fgPrimary, gfx::VAlign::Center);
    const float sy = tr.bottom() + 2;
    const float x = cl.circle ? r.x : paintMarks(c, cl, r.x, sy + 8);
    c.text(tx.sub, {x, sy, std::max(0.f, r.right() - x), 16}, col.fgSecondary, gfx::VAlign::Center);
    c.popTransform();
}

void LibraryView::paintRow(Canvas& c, int d, const Rect& r) {
    const auto& col = colors();
    const auto& acc = accent();
    const LibraryCell& cl = cell(d);
    const float h = hoverOf(d);
    c.fillRounded(r, gfx::radii().surface, col.overlayHover.mulAlpha(h));
    if (d == dropHot_) {
        c.fillRounded(r, gfx::radii().surface, acc.tint12);
        c.strokeRounded(r, gfx::radii().surface, acc.base);
    }
    const Rect art = artRect(d);
    paintArt(c, cl, art, gfx::radii().art);
    if (cl.playable && pagetx::presence(&cl) > 0.f) paintPlay(c, d, cl, playRect(d), h, 14);
    auto& tx = text(d);
    if (!tx.metaDone) {
        tx.metaDone = true;
        if (metaFor) tx.meta.setText(metaFor(order_[static_cast<size_t>(d)]));
    }
    const bool current = isCurrent(cl);
    float right = r.right() - 12;
    if (!tx.meta.empty()) {
        const float mw = std::ceil(tx.meta.measure().w);
        c.text(tx.meta, {right - mw, r.y, mw + 1, r.h}, col.fgTertiary, gfx::VAlign::Center);
        right -= mw + 20;
    }
    if (current && ctx().player->isPlaying()) {   // like the sidebar's playing row: the live bars
        drawPlayingBars(c, this, {right - 14, r.cy() - 7, 14, 14}, acc.base);
        right -= 14 + 12;
    }
    const float tx0 = art.right() + 14;
    c.text(tx.title, {tx0, r.cy() - 19, std::max(0.f, right - tx0), 20}, current ? acc.base : col.fgPrimary,
           gfx::VAlign::Center);
    const float x = paintMarks(c, cl, tx0, r.cy() + 10);
    c.text(tx.sub, {x, r.cy() + 1, std::max(0.f, right - x), 18}, col.fgSecondary, gfx::VAlign::Center);
    c.hline(tx0, r.right(), r.bottom() - 1, col.hairSubtle);
}

void LibraryView::paint(Canvas& c) {
    const int n = static_cast<int>(order_.size());
    if (n == 0) return;
    const Rect r = rect();
    const Rect vis = c.clip();   // only what intersects the visible clip is painted
    if (mode_ == Mode::List) {
        const int first = std::max(0, static_cast<int>((vis.y - r.y) / kRowH));
        const int last = std::min(n - 1, static_cast<int>((vis.bottom() - r.y) / kRowH));
        for (int d = first; d <= last; ++d) paintRow(c, d, cellRect(d));
        return;
    }
    const int cols = columns(r.w);
    const float pitch = cellHeight(r.w) + kGap;
    const int firstRow = std::max(0, static_cast<int>((vis.y - r.y) / pitch));
    const int lastRow = static_cast<int>((vis.bottom() - r.y) / pitch);
    for (int row = firstRow; row <= lastRow; ++row)
        for (int col = 0; col < cols; ++col) {
            const int d = row * cols + col;
            if (d >= n) return;
            paintCard(c, d, cellRect(d));
        }
}

// ---- Input ------------------------------------------------------------------------------------------------------------

void LibraryView::setHot(int d, bool playHot) {
    if (d != hot_) {
        prevHot_ = hot_;
        prevA_.snap(hotA_.value());
        prevA_.to(0, ui::motion::microMs(ui::motion::fast));
        hot_ = d;
        hotA_.snap(0);
        if (d >= 0) hotA_.to(1, ui::motion::microMs(ui::motion::fast));
        playHotA_.snap(0);
        playHot_ = false;
    }
    if (playHot != playHot_) playHotA_.to(playHot ? 1.f : 0.f, ui::motion::microMs(ui::motion::fast));
    playHot_ = playHot;
    invalidate();
}

void LibraryView::onMouseMove(const ui::MouseEvent& e) {
    const int d = cellAt(e.pos);
    const bool ph = d >= 0 && cell(d).playable && !(mode_ == Mode::Grid && cell(d).circle) && playRect(d).contains(e.pos);
    if (d != hot_ || ph != playHot_) setHot(d, ph);
}

void LibraryView::onMouseLeave() {
    if (hot_ >= 0 || playHot_) setHot(-1, false);
}

bool LibraryView::onMouseDown(const ui::MouseEvent& e) {
    const int d = cellAt(e.pos);
    if (e.button == ui::MouseButton::Right) {
        if (d >= 0) {
            cursor_ = d;
            context(d, e.windowPos);
        }
        return false;
    }
    if (e.button != ui::MouseButton::Left || d < 0) return false;
    pressed_ = d;
    pressPlay_ = cell(d).playable && !(mode_ == Mode::Grid && cell(d).circle) && playRect(d).contains(e.pos);
    (pressPlay_ ? playPressA_ : pressA_).to(1, ui::motion::microMs(80));
    invalidate();
    return true;
}

std::optional<PreviewHit> LibraryView::previewAt(gfx::Point windowPos, bool keyboard) {
    if (order_.empty()) return std::nullopt;
    int d = std::clamp(cursor_, 0, static_cast<int>(order_.size()) - 1);
    if (!keyboard) {
        const gfx::Point p = fromWindow(windowPos);
        d = cellAt(p);
        if (d < 0) return std::nullopt;
        const LibraryCell& c = cell(d);
        if (c.playable && !(mode_ == Mode::Grid && c.circle) && playRect(d).contains(p)) return std::nullopt;
        if (mode_ == Mode::Grid && !artRect(d).contains(p)) return std::nullopt;   // a card: its cover
    }
    const LibraryCell& c = cell(d);
    // The cell's player context names it ("liked", "playlist:<id>", "album:<id>", "artist:<id>"; folders: none).
    auto target = preview::contextTarget(c.context, c.title, c.images);
    if (!target) return std::nullopt;
    return PreviewHit{std::move(*target), toWindow(mode_ == Mode::Grid ? artRect(d) : cellRect(d))};
}

void LibraryView::onMouseUp(const ui::MouseEvent& e) {
    pressA_.to(0, ui::motion::microMs(160));
    playPressA_.to(0, ui::motion::microMs(160));
    invalidate();
    const int d = cellAt(e.pos);
    if (d < 0 || d != pressed_) return;
    cursor_ = d;
    const LibraryCell& c = cell(d);
    if (c.playable && !(mode_ == Mode::Grid && c.circle) && playRect(d).contains(e.pos)) play(d);
    else open(d);
}

void LibraryView::open(int d) {
    if (!onOpen) return;
    pagetx::arm(coverOf(d));
    const auto fn = onOpen;   // may navigate (this view is deleted after the event)
    fn(order_[static_cast<size_t>(d)]);
}

void LibraryView::play(int d) {
    if (!cell(d).playable || !onPlay) return open(d);
    pagetx::arm(coverOf(d));   // "#play" opens the page too
    const auto fn = onPlay;
    fn(order_[static_cast<size_t>(d)]);
}

SharedCover LibraryView::coverOf(int d) const {
    const LibraryCell& cl = cell(d);
    const Rect art = artRect(d);
    const bool round = cl.circle;
    SharedCover s = cl.likedTile && cl.images.empty() ? pagetx::likedCover(*this, art, 2)
                                                      : pagetx::imageCover(*this, art, round ? art.w * 0.5f : 2.f, cl.images);
    s.owner = &cl;   // one view, many covers: the cell is the owner
    return s;
}

bool LibraryView::coverFor(const std::string& key, SharedCover& out) {
    if (key.empty() || !cells_) return false;
    for (int d = 0; d < static_cast<int>(order_.size()); ++d) {
        const LibraryCell& cl = cell(d);
        const bool match = cl.likedTile && cl.images.empty() ? key == "liked" : !cl.images.empty() && cl.images.front().url == key;
        if (!match) continue;
        out = coverOf(d);
        return true;
    }
    return false;
}

void LibraryView::context(int d, gfx::Point windowPos) {
    if (!onContext) return;
    invalidate();
    const auto fn = onContext;
    fn(order_[static_cast<size_t>(d)], windowPos);
}

bool LibraryView::onKeyDown(const ui::KeyEvent& e) {
    const int n = static_cast<int>(order_.size());
    if (n == 0 || e.alt) return false;
    const bool grid = mode_ == Mode::Grid;
    const int cols = columns(rect().w);
    int cur = std::clamp(cursor_, 0, n - 1);
    if (e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) {
        cursor_ = cur;
        const Rect a = toWindow(grid ? artRect(cur) : cellRect(cur));
        context(cur, grid ? gfx::Point{a.x + 12, a.bottom() - 12} : gfx::Point{a.x + 56, a.bottom()});
        return true;
    }
    if (e.ctrl) return false;   // Ctrl+arrows / Ctrl+Enter are the app's shortcuts
    // A page of rows: what fits in the scrolling viewport.
    int page = grid ? 3 : 10;
    if (const auto* sv = scrollViewOf(this))
        page = std::max(1, static_cast<int>(sv->rect().h / (grid ? cellHeight(rect().w) + kGap : kRowH)) - 1);
    switch (e.vk) {
    case VK_LEFT:
        if (!grid) return false;
        cur = std::max(0, cur - 1);
        break;
    case VK_RIGHT:
        if (!grid) return false;
        cur = std::min(n - 1, cur + 1);
        break;
    case VK_UP:
        if (cur >= cols) cur -= cols;
        break;
    case VK_DOWN:
        if (cur + cols < n) cur += cols;
        else if (cur / cols < (n - 1) / cols) cur = n - 1;   // into the shorter last row
        break;
    case VK_PRIOR: cur = std::max(cur % cols, cur - cols * page); break;
    case VK_NEXT: cur = std::min(n - 1, cur + cols * page); break;
    case VK_HOME: cur = 0; break;
    case VK_END: cur = n - 1; break;
    case VK_RETURN:
        cursor_ = cur;
        if (e.shift) play(cur);
        else open(cur);
        return true;
    default: return false;
    }
    cursor_ = cur;
    invalidate();
    return true;
}

// ---- Drops ------------------------------------------------------------------------------------------------------------

bool LibraryView::dragOver(const DragPayload& payload, gfx::Point windowPos) {
    autoScroll(windowPos);
    int hot = -1;
    if (!payload.fromExplorer()) {
        const int d = cellAt(fromWindow(windowPos));
        if (d >= 0 && cell(d).drop && !dragdrop::tracksForRow(*cell(d).drop, payload.tracks).empty()) hot = d;
    }
    if (hot != dropHot_) {
        dropHot_ = hot;
        invalidate();
    }
    return hot >= 0;
}

void LibraryView::dragLeave() {
    lastScrollTick_ = 0;
    if (dropHot_ >= 0) {
        dropHot_ = -1;
        invalidate();
    }
}

void LibraryView::drop(const DragPayload& payload, gfx::Point windowPos) {
    const int d = cellAt(fromWindow(windowPos));
    dragLeave();
    if (d < 0 || payload.fromExplorer() || !cell(d).drop) return;
    // After the event: adding the songs updates the library, and the page rebuilds this view.
    Dispatcher::post([target = *cell(d).drop, tracks = payload.tracks] { dragdrop::dropOnRow(target, tracks); });
}

// Scrolls the page while dragged songs rest near its top / bottom edge (as the track table does).
void LibraryView::autoScroll(gfx::Point windowPos) {
    const double now = ui::frame::realNow(), last = lastScrollTick_;
    lastScrollTick_ = now;
    if (last <= 0) return;
    ui::ScrollView* sv = scrollViewOf(this);
    if (!sv) return;
    const Rect vr = sv->toWindow(sv->rect());
    const float top = vr.y + sv->topInset, bottom = vr.bottom(), edge = 48, y = windowPos.y;
    float v = 0;
    if (y >= top && y < top + edge) v = -(1 - (y - top) / edge);
    else if (y > bottom - edge && y <= bottom) v = 1 - (bottom - y) / edge;
    if (v == 0) return;
    const float dt = static_cast<float>(std::min(now - last, 100.0)) / 1000.f;
    sv->scrollTo(std::clamp(sv->scrollY() + v * 900.f * dt, 0.f, sv->maxScroll()), false);
}

// ---- Recently played collections ----------------------------------------------------------------------------------------

namespace {

struct RecentsState {
    libview::Recents recents;
    bool dirty = false;
    int64_t lastSave = 0;   // steadyMs()
};
RecentsState& recentsState() {
    static RecentsState s;
    return s;
}
std::mutex& saveMutex() {   // the exit save and a background one never write the file at once
    static std::mutex m;
    return m;
}
std::filesystem::path recentsFile() { return accounts::file(L"library-recents.json"); }   // per account (app/Accounts)

// Housekeeping: at most every 30 s, written on a worker; on exit (`now`) at once.
void saveRecents(bool now) {
    auto& s = recentsState();
    if (!s.dirty) return;
    const int64_t t = steadyMs();
    if (!now && t - s.lastSave < 30'000) return;
    s.dirty = false;
    s.lastSave = t;
    auto write = [copy = s.recents, file = recentsFile()] {
        std::lock_guard lock(saveMutex());
        if (!copy.save(file)) ST_LOG_WARN("library", "could not write library-recents.json");
    };
    if (now) write();
    else background(Priority::Low, std::move(write));
}

} // namespace

const libview::Recents& libraryRecents() { return recentsState().recents; }

const libview::PlayRecency& libraryPlayRecency() {
    static libview::PlayRecency recency;
    static size_t plays = SIZE_MAX, tracks = 0;
    static int64_t newest = 0;
    const auto& stats = listenStats();
    const size_t n = stats.playCount();
    const int64_t last = n ? stats.plays().back().startedAt : 0;
    if (n != plays || last != newest || stats.tracks().size() != tracks) {
        recency.build(stats);
        plays = n;
        newest = last;
        tracks = stats.tracks().size();
    }
    return recency;
}

void initLibraryViews() {
    recentsState().recents = libview::Recents::load(recentsFile());
    // A song of a library collection started: that collection was just played ("Son çalınanlar").
    ctx().trackChangedHooks.push_back([](const catalog::Track&) {
        const auto* p = ctx().player;
        if (p && recentsState().recents.record(p->context().uri, nowUnix())) recentsState().dirty = true;
    });
    ctx().housekeepingHooks.push_back([] { saveRecents(false); });
    ctx().persistHooks.push_back([] { saveRecents(true); });
    // Another Spotify account: what this one played goes to its folder, the other one's comes back.
    ctx().accountLeaveHooks.push_back([] { saveRecents(true); });
    ctx().accountEnterHooks.push_back([] {
        recentsState().recents = libview::Recents::load(recentsFile());
        recentsState().dirty = false;
    });
}

} // namespace st::app
