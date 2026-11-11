#include "app/LibraryView.h"

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

float LibraryView::hoverOf(int d) const {
    float h = 0;
    if (d == hot_) h = hotA_.value();
    if (d == prevHot_) h = std::max(h, prevA_.value());
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

void LibraryView::paintPlay(Canvas& c, const LibraryCell& cell, const Rect& pr, float h, float iconSize) {
    const auto& acc = accent();
    const bool playing = isCurrent(cell) && ctx().player->isPlaying();
    const bool hot = playHot_ && hot_ >= 0 && &this->cell(hot_) == &cell;
    c.pushOpacity(h);
    c.fillCircle({pr.cx(), pr.cy()}, pr.w * 0.5f, hot ? acc.hover : acc.base);
    c.icon(playing ? "pause" : "play", pr.center(iconSize, iconSize).offset(playing ? 0.f : 1.f, 0), acc.onAccent);
    c.popLayer();
}

void LibraryView::paintCard(Canvas& c, int d, const Rect& r) {
    const auto& col = colors();
    const auto& acc = accent();
    const LibraryCell& cl = cell(d);
    const float h = hoverOf(d);
    const Rect art{r.x, r.y, r.w, r.w};
    const float s = 1 - 0.01f * (d == pressed_ ? pressA_.value() : 0.f);
    c.pushScale(s, {art.cx(), art.cy()});
    if (cl.circle) {
        paintArt(c, cl, art, 0);
        if (h > 0.01f) c.strokeCircle({art.cx(), art.cy()}, art.w * 0.5f, acc.base.mulAlpha(h * 0.8f), 1.5f);
        if (d == dropHot_) c.strokeCircle({art.cx(), art.cy()}, art.w * 0.5f, acc.base, 2.f);
    } else {
        if (h > 0.01f) c.shadow(art, 2, 24, 12, col.shadowCard.mulAlpha(h));   // cardHoverShadow
        paintArt(c, cl, art, 2);
        if (d == dropHot_) {   // dragged songs would go here: the sidebar row's drop look
            c.fillRounded(art, 2, acc.tint12);
            c.strokeRounded(art, 2, acc.base);
        }
        if (cl.playable && h > 0.01f)
            paintPlay(c, cl, playRect(d).offset(0, 8 * (1 - ui::ease(ui::Ease::Decelerate, h))), h, 16);
    }
    c.popTransform();
    auto& tx = text(d);
    const Rect tr{r.x, art.bottom() + 10, r.w, 18};
    c.text(tx.title, tr, isCurrent(cl) ? acc.base : col.fgPrimary, gfx::VAlign::Center);
    const float sy = tr.bottom() + 2;
    const float x = cl.circle ? r.x : paintMarks(c, cl, r.x, sy + 8);
    c.text(tx.sub, {x, sy, std::max(0.f, r.right() - x), 16}, col.fgSecondary, gfx::VAlign::Center);
}

void LibraryView::paintRow(Canvas& c, int d, const Rect& r) {
    const auto& col = colors();
    const auto& acc = accent();
    const LibraryCell& cl = cell(d);
    const float h = hoverOf(d);
    c.fillRounded(r, 2, col.overlayHover.mulAlpha(h));
    if (d == dropHot_) {
        c.fillRounded(r, 2, acc.tint12);
        c.strokeRounded(r, 2, acc.base);
    }
    const Rect art = artRect(d);
    paintArt(c, cl, art, 2);
    if (cl.playable && h > 0.01f) paintPlay(c, cl, playRect(d), h, 14);
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
    if (current && ctx().player->isPlaying()) {   // like the sidebar's playing row
        c.icon("equalizer", {right - 14, r.cy() - 7, 14, 14}, acc.base);
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
        prevA_.to(0, ui::motion::fast);
        hot_ = d;
        hotA_.snap(0);
        if (d >= 0) hotA_.to(1, ui::motion::fast);
    }
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
    pressA_.to(1, 80);
    invalidate();
    return true;
}

void LibraryView::onMouseUp(const ui::MouseEvent& e) {
    pressA_.to(0, 160);
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
    const auto fn = onOpen;   // may navigate (this view is deleted after the event)
    fn(order_[static_cast<size_t>(d)]);
}

void LibraryView::play(int d) {
    if (!cell(d).playable || !onPlay) return open(d);
    const auto fn = onPlay;
    fn(order_[static_cast<size_t>(d)]);
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
std::filesystem::path recentsFile() { return paths::appData() / L"library-recents.json"; }

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
}

} // namespace st::app
