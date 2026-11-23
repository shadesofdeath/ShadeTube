#include "app/HomeWidgets.h"

#include "app/Source.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app::home {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;
namespace metrics = gfx::metrics;

int columns(float width) {
    const float minCard = Settings::get().compactLists ? kMinCardCompact : kMinCard;
    return std::max(2, static_cast<int>((width + kCardGap) / (minCard + kCardGap)));
}

float cardWidth(float width) {
    const int n = columns(width);
    return std::max(60.f, std::floor((width - kCardGap * static_cast<float>(n - 1)) / static_cast<float>(n)));
}

namespace {

// The artwork of an item: its own painter, the Liked Songs tile, a page's icon, or the images.
void drawItemArt(Canvas& c, const Item& it, const Rect& r) {
    const auto& col = colors();
    if (it.drawArt) {
        it.drawArt(c, r);
    } else if (it.likedTile) {
        c.fillRounded(r, metrics::radiusSurface, accent().base);
        const float g = std::round(std::min(r.w, r.h) * 0.36f);
        c.icon("heart-filled", r.center(g, g), accent().onAccent);
    } else if (!it.icon.empty()) {
        c.fillRounded(r, metrics::radiusSurface, gfx::isLightTheme() ? col.bgSunken : col.bgOverlay);
        const float g = std::round(std::min(r.w, r.h) * 0.4f);
        c.icon(it.icon, r.center(g, g), col.fgSecondary);
    } else if (it.circle) {
        drawArtwork(c, it.images, r, 0, Placeholder::Artist, true, Priority::Normal);
    } else {
        drawArtwork(c, it.images, r, metrics::radiusSurface, it.placeholder, false, Priority::Normal);
    }
}

bool isPlayingContext(const std::string& context) {
    const auto* p = ctx().player;
    return !context.empty() && p && p->current() && p->isPlaying() && p->context().uri == context;
}

// The headline's two parts: before / after the "\n" of greeting() (a translation without one is all bold).
std::pair<std::wstring, std::wstring> greetingLines() {
    const std::wstring g = greeting();
    const size_t nl = g.find(L'\n');
    if (nl == std::wstring::npos) return {g, std::wstring()};
    return {g.substr(0, nl), g.substr(nl + 1)};
}

} // namespace

// =====================================================================================================================
// Greeting

Greeting::Greeting(std::wstring meta1, std::wstring meta2)
    : meta1_(std::move(meta1), type::monoLabel), meta2_(std::move(meta2), type::monoLabel) {
    hitTestVisible = false;
    const auto [a, b] = greetingLines();
    line1_ = gfx::Text(a, type::displayM);
    line2_ = gfx::Text(b, type::displayM.withWeight(300));
}

float Greeting::size(float width) const { return width >= 900 ? 56.f : width >= 560 ? 44.f : 36.f; }

// One line when "bold light" fits next to the meta column (wide pages) / in the width (narrow ones).
bool Greeting::oneLine(float width) {
    const float s = size(width);
    line1_.setStyle(type::displayM.withSize(s));
    line2_.setStyle(type::displayM.withSize(s).withWeight(300));
    const float metaW = width >= 900 ? std::max(meta1_.measure().w, meta2_.measure().w) + 40 : 0.f;
    const float need = line1_.measure().w + s * 0.24f + (line2_.empty() ? 0.f : line2_.measure().w);
    return need <= width - metaW;
}

float Greeting::preferredHeight(float width) {
    const float s = size(width);
    const float lines = oneLine(width) || line2_.empty() ? 1.f : 2.f;
    const float metaBelow = width >= 900 ? 0.f : 44.f;
    return std::ceil(lines * s * 1.02f + 10 + metaBelow);
}

void Greeting::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const float s = size(r.w);
    const bool one = oneLine(r.w);
    const float lh = std::ceil(s * 1.02f);
    const float w1 = std::ceil(line1_.measure().w);
    c.text(line1_, {r.x - 2, r.y, r.w, lh + 12}, col.fgPrimary);
    if (!line2_.empty()) {
        if (one) c.text(line2_, {r.x - 2 + w1 + std::round(s * 0.24f), r.y, r.w, lh + 12}, col.fgPrimary);
        else c.text(line2_, {r.x - 2, r.y + lh, r.w, lh + 12}, col.fgPrimary);
    }
    // Mono meta: right-aligned on the headline's last line (wide), else under it.
    const float mw = std::ceil(std::max(meta1_.measure().w, meta2_.measure().w)) + 1;
    if (r.w >= 900) {
        const float bottom = r.y + (one || line2_.empty() ? lh : 2 * lh);
        c.text(meta1_, {r.right() - std::ceil(meta1_.measure().w) - 1, bottom - 34, mw, 14}, col.fgTertiary);
        c.text(meta2_, {r.right() - std::ceil(meta2_.measure().w) - 1, bottom - 16, mw, 14}, col.fgTertiary);
    } else {
        const float y = r.bottom() - 34;
        c.text(meta1_, {r.x, y, r.w, 14}, col.fgTertiary);
        c.text(meta2_, {r.x, y + 18, r.w, 14}, col.fgTertiary);
    }
}

// =====================================================================================================================
// Hero

Hero::Hero(std::function<std::optional<Item>()> source)
    : source_(std::move(source)), label_({}, type::monoLabel), title_({}, type::displayS), subtitle_({}, type::bodyRegular),
      progress_({}, type::monoLabel) {
    focusable = true;
    gfx::TextOptions two;
    two.wrap = true;
    two.maxLines = 2;
    title_.setOptions(two);
    play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
    play_->onClick = [this] {
        if (!item_ || !item_->play) return;
        const auto play = item_->play;   // may navigate (this page goes away)
        play();
    };
    welcome_ = add<Welcome>();
    refresh();
}

std::string Hero::playerKey() const {
    const auto* p = ctx().player;
    const auto* t = p ? p->current() : nullptr;
    return t ? p->context().uri + "|" + t->id : std::string();
}

void Hero::refresh() {
    const auto oldUrl = item_ && !item_->images.empty() ? item_->images.front().url : std::string();
    item_ = source_ ? source_() : std::nullopt;
    key_ = playerKey();
    const auto newUrl = item_ && !item_->images.empty() ? item_->images.front().url : std::string();
    if (!item_ || newUrl != oldUrl || newUrl.empty()) {   // another cover: its color fades in again
        tintColor_.reset();
        tint_.snap(0);
    }
    if (item_) {
        label_.setText(item_->label);
        title_.setText(item_->title);
        subtitle_.setText(item_->subtitle);
        progress_.setText(item_->progressLabel);
        play_->setTooltip(item_->current ? std::wstring() : std::wstring(tr(L"Çal")));
    }
    play_->setVisible(item_ && item_->play);
    welcome_->setVisible(!item_);
    focusable = item_.has_value();
    requestLayout();
    invalidate();
}

float Hero::preferredHeight(float width) {
    if (!item_) return welcome_->preferredHeight(width);
    return width >= 720 ? 216.f : width >= 480 ? 176.f : 148.f;
}

Rect Hero::artRect() const {
    const Rect r = rect();
    const float pad = r.h >= 216 ? 24.f : 20.f;
    const float s = r.h - 2 * pad;
    return {r.x + pad, r.y + pad, s, s};
}

void Hero::layout() {
    const Rect r = rect();
    const float s = r.h >= 176 ? 56.f : 48.f;
    play_->setRect({r.w - 28 - s, std::round((r.h - s) * 0.5f), s, s});
    welcome_->setRect({0, 0, r.w, r.h});
}

void Hero::paint(Canvas& c) {
    // The player moved on (another list, another track): ask again, after this frame.
    if (!refreshPosted_ && playerKey() != key_) {
        refreshPosted_ = true;
        Dispatcher::post([this, ref = life_.ref()] {
            if (ref.expired()) return;
            refreshPosted_ = false;
            refresh();
        });
    }
    if (!item_) {
        paintChildren(c);   // the welcome card
        return;
    }
    const Rect r = rect();
    const auto& col = colors();
    const auto& acc = accent();
    const bool light = gfx::isLightTheme();
    auto* p = ctx().player;
    const Item& it = *item_;
    play_->setPlaying(it.current && p && p->isPlaying());
    play_->setLoading(it.current && p && p->status() == player::Status::Resolving);
    if (it.current && p) {   // the player's item: "now playing" while it plays, "continue" while it is paused
        std::wstring label = p->isPlaying() ? std::wstring(tr(L"ŞİMDİ ÇALIYOR")) : toUpperTr(tr(L"Kaldığın yerden devam et"));
        if (label != label_.text()) label_.setText(std::move(label));
    }

    // The cover's color: known once the image has been decoded (fades in), or given by the item.
    const Rect art = artRect();
    if (!tintColor_) {
        if (it.tint) {
            tintColor_ = it.tint;
        } else if (it.likedTile) {
            tintColor_ = acc.base;
        } else {
            const int px = static_cast<int>(std::ceil(art.w * c.scale()));
            if (const auto* img = catalog::pickImage(it.images, px)) tintColor_ = gfx::ImageCache::get().accentOf(img->url);
        }
        if (tintColor_) tint_.to(1, ui::motion::slow, ui::Ease::Standard);
    }
    // Opaque card (Mica stays behind it), a soft gradient of the cover's color from behind the artwork, a hairline.
    c.fillRounded(r, metrics::radiusSurface, col.bgRaised);
    if (tintColor_) {
        const float a = (light ? 0.22f : 0.34f) * tint_.value();
        const Color tc = *tintColor_;
        c.fillLinearGradient(r, {r.x, r.y}, {r.x + r.w * 0.78f, r.y + r.h * 0.35f}, tc.withAlpha(a), tc.withAlpha(0));
    }
    c.strokeRounded(r, metrics::radiusSurface, Color::lerp(col.hairDefault, col.hairStrong, hover_.value()));
    drawItemArt(c, it, art);

    // Text column between the artwork and the play button, centered on the artwork.
    const float tx = art.right() + (r.h >= 216 ? 32.f : 24.f);
    const float right = play_->visible() ? r.x + play_->rect().x - 24 : r.right() - 24;
    const float tw = std::max(0.f, right - tx);
    title_.setStyle(type::displayS.withSize(r.w >= 720 ? 40.f : r.w >= 480 ? 30.f : 24.f));
    const float th = std::ceil(title_.measure(tw).h);
    const bool hasSub = !subtitle_.empty();
    float progress = it.progress;
    std::wstring progressText = it.progressLabel;
    if (it.current && progress >= 0 && p && p->durationMs() > 0) {   // the episode playing now: live
        progress = std::clamp(static_cast<float>(p->positionMs()) / static_cast<float>(p->durationMs()), 0.f, 1.f);
        const int64_t left = std::max<int64_t>(0, p->durationMs() - p->positionMs());
        progressText = toUpperTr(i18n::plural(L"{} dk kaldı", static_cast<size_t>((left + 59999) / 60000)));
    }
    const bool hasProgress = progress >= 0;
    const float block = 14 + 12 + th + (hasSub ? 8 + 20 : 0) + (hasProgress ? 16 + 14 : 0);
    float y = std::round(art.y + (art.h - block) * 0.5f);
    c.text(label_, {tx, y, tw, 14}, col.fgSecondary);
    y += 14 + 12;
    c.text(title_, {tx - 1, y, tw, th + 4}, col.fgPrimary);
    y += th;
    if (hasSub) {
        y += 8;
        c.text(subtitle_, {tx, y, tw, 20}, col.fgSecondary, gfx::VAlign::Center);
        y += 20;
    }
    if (hasProgress) {
        y += 16;
        const float pw = std::min(tw, 240.f);
        c.fillRounded({tx, y + 6, pw, 2}, 1, col.hairStrong);
        c.fillRounded({tx, y + 6, std::max(2.f, pw * progress), 2}, 1, acc.base);
        if (!progressText.empty()) {
            progress_.setText(progressText);
            c.text(progress_, {tx + pw + 12, y, std::max(0.f, tw - pw - 12), 14}, col.fgTertiary);
        }
    }
    paintChildren(c);
}

bool Hero::onMouseDown(const ui::MouseEvent& e) { return e.button == ui::MouseButton::Left && item_ && item_->open; }

void Hero::onMouseUp(const ui::MouseEvent& e) {
    if (!hovered() || !rect().contains(e.pos) || !item_ || !item_->open) return;
    const auto open = item_->open;   // navigates: this page goes away after the event
    open();
}

void Hero::onMouseEnter() {
    hover_.to(1, ui::motion::fast);
    invalidate();
}

void Hero::onMouseLeave() {
    hover_.to(0, ui::motion::fast);
    invalidate();
}

bool Hero::onActivate() {
    if (!item_ || !item_->open) return false;
    const auto open = item_->open;
    open();
    return true;
}

// =====================================================================================================================
// Welcome

Welcome::Welcome()
    : title_(tr(L"Müziğe buradan başla"), type::displayS.withSize(32)),
      body_(tr(L"Bir şarkı ara, bir radyo istasyonu aç ya da kendi müzik klasörünü ekle. Çaldıkların burada, kaldığın "
               L"yerde seni bekler."),
            type::bodyRegular) {
    gfx::TextOptions wrap;
    wrap.wrap = true;
    wrap.maxLines = 3;
    body_.setOptions(wrap);
    auto button = [this](ButtonKind kind, std::wstring label, const char* icon, std::function<void()> fn) {
        auto* b = add<Button>(kind, std::move(label), icon);
        b->onClick = std::move(fn);
        buttons_.push_back(b);
    };
    button(ButtonKind::Primary, tr(L"Ara"), "search", [] { ctx().router->navigate({RouteKind::Search}); });
    if (!source::loggedIn() && ctx().startSpotifyLogin)
        button(ButtonKind::Secondary, tr(L"Spotify ile bağlan"), "spotify-link", [] {
            if (ctx().startSpotifyLogin) ctx().startSpotifyLogin();
        });
    button(ButtonKind::Secondary, tr(L"Yerel dosyalar"), "music-note", [] { ctx().router->navigate({RouteKind::LocalFiles}); });
    button(ButtonKind::Secondary, tr(L"Radyo"), "radio", [] { ctx().router->navigate({RouteKind::Radio}); });
}

float Welcome::preferredHeight(float width) {
    const float tw = std::max(0.f, std::min(width - 64, 620.f));
    const float bh = std::ceil(body_.measure(tw).h);
    // Buttons wrap to a second row on a narrow page.
    float x = 0, rows = 1;
    for (auto* b : buttons_) {
        const float w = std::ceil(b->naturalWidth());
        if (x > 0 && x + w > width - 64) {
            ++rows;
            x = 0;
        }
        x += w + 8;
    }
    return 32 + 34 + 12 + bh + 24 + rows * 40 + (rows - 1) * 8 + 32;
}

void Welcome::layout() {
    const Rect r = rect();
    const float tw = std::max(0.f, std::min(r.w - 64, 620.f));
    float y = 32 + 34 + 12 + std::ceil(body_.measure(tw).h) + 24;
    float x = 32;
    for (auto* b : buttons_) {
        const float w = std::ceil(b->naturalWidth());
        if (x > 32 && x + w > r.w - 32) {
            x = 32;
            y += 48;
        }
        b->setRect({x, y, w, 40});
        x += w + 8;
    }
}

void Welcome::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.fillRounded(r, metrics::radiusSurface, col.bgRaised);
    c.fillLinearGradient(r, {r.x, r.y}, {r.x + r.w * 0.7f, r.y + r.h * 0.4f},
                         accent().base.withAlpha(gfx::isLightTheme() ? 0.12f : 0.10f), accent().base.withAlpha(0));
    c.strokeRounded(r, metrics::radiusSurface, col.hairDefault);
    const float tw = std::max(0.f, std::min(r.w - 64, 620.f));
    c.text(title_, {r.x + 31, r.y + 32, r.w - 64, 40}, col.fgPrimary);
    c.text(body_, {r.x + 32, r.y + 32 + 34 + 12, tw, 80}, col.fgSecondary);
    paintChildren(c);
}

// =====================================================================================================================
// Quick access

namespace {

class QuickTile : public ui::Widget {
public:
    explicit QuickTile(Item item) : item_(std::move(item)), title_(item_.title, type::body.withSize(13)) {
        focusable = true;
        gfx::TextOptions two;
        two.wrap = true;
        two.maxLines = 2;
        title_.setOptions(two);
    }

    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        const float h = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        c.pushScale(1 - 0.01f * press_, {r.cx(), r.cy()});
        c.fillRounded(r, metrics::radiusSurface, col.overlayHover.mulAlpha(h));
        c.strokeRounded(r.inset(0.5f), metrics::radiusSurface, Color::lerp(col.hairDefault, col.hairStrong, h));
        const Rect art{r.x, r.y, r.h, r.h};
        if (item_.circle) drawItemArt(c, item_, art.inset(6));
        else drawItemArt(c, item_, art);
        const Rect pb = playRect();
        float textRight = r.right() - 12;
        if (isPlayingContext(item_.context)) {
            c.icon("equalizer", pb.center(16, 16), acc.base);
            textRight = pb.x - 6;
        } else if (item_.play) {
            if (h > 0.01f) {
                c.fillCircle({pb.cx(), pb.cy()}, pb.w * 0.5f, (playHot_ ? acc.hover : acc.base).mulAlpha(h));
                c.icon("play", pb.center(12, 12).offset(1, 0), acc.onAccent.mulAlpha(h));
            }
            textRight = pb.x - 6;
        }
        const float tx = art.right() + 12;
        titleW_ = std::max(0.f, textRight - tx);
        const float th = std::min(std::ceil(title_.measure(titleW_).h), r.h);
        c.text(title_, {tx, std::round(r.cy() - th * 0.5f), titleW_, th + 2}, col.fgPrimary);
        c.popTransform();
    }

    std::wstring tooltip() const override {
        auto& t = const_cast<gfx::Text&>(title_);
        return titleW_ > 0 && t.measure(titleW_).h > rect().h - 4 ? item_.title : std::wstring();
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        press_.to(1, 80);
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, 160);
        if (!rect().contains(e.pos)) return;
        const bool play = item_.play && playRect().contains(e.pos) && !isPlayingContext(item_.context);
        const auto fn = play ? item_.play : item_.open;   // navigating destroys this tile: call a copy
        if (fn) fn();
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = item_.play && playRect().contains(e.pos);
        if (hot != playHot_) {
            playHot_ = hot;
            invalidate();
        }
    }
    void onMouseEnter() override {
        hover_.to(1, ui::motion::fast);
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::fast);
        playHot_ = false;
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!item_.open) return false;
        press_.snap(1);
        press_.to(0, 160);
        const auto open = item_.open;
        open();
        return true;
    }
    // Shift+Enter plays it (as in Kitaplık).
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_RETURN || !e.shift || e.ctrl || e.alt || !item_.play) return false;
        const auto play = item_.play;
        play();
        return true;
    }

private:
    Rect playRect() const {
        const Rect r = rect();
        constexpr float s = 28;
        return {r.right() - 10 - s, std::round(r.cy() - s * 0.5f), s, s};
    }

    Item item_;
    gfx::Text title_;
    ui::Anim hover_, press_;
    bool playHot_ = false;
    float titleW_ = 0;
};

float quickTileH() { return Settings::get().compactLists ? 48.f : 56.f; }

} // namespace

QuickGrid::QuickGrid(std::vector<Item> items) {
    hitTestVisible = false;
    for (auto& it : items) add<QuickTile>(std::move(it));
}

int QuickGrid::cols(float width) const { return width >= 880 ? 4 : width >= 600 ? 3 : 2; }
int QuickGrid::rows(float width) const {
    const int c = cols(width);
    const int n = std::min(static_cast<int>(children().size()), c == 2 ? 6 : c * 2);
    return (n + c - 1) / c;
}

float QuickGrid::preferredHeight(float width) {
    const int r = rows(width);
    return r <= 0 ? 0.f : r * quickTileH() + (r - 1) * metrics::gridGap;
}

void QuickGrid::layout() {
    const Rect r = rect();
    const int c = cols(r.w);
    const int shown = rows(r.w) * c;
    const float w = std::floor((r.w - metrics::gridGap * (c - 1)) / c);
    const float h = quickTileH();
    int i = 0;
    for (auto& child : children()) {
        const bool on = i < shown;
        child->setVisible(on);
        if (on) child->setRect({(i % c) * (w + metrics::gridGap), (i / c) * (h + metrics::gridGap), w, h});
        ++i;
    }
}

// =====================================================================================================================
// Shelf

// The scrolling part: the cards side by side, starting kBleed in (the track reaches into the page's gutters, where the
// edge fade happens and the cards' hover shadows have room).
class ShelfTrack : public ui::Widget {
public:
    ShelfTrack() {
        clipsChildren = true;
        hitTestVisible = true;   // the gaps between cards still belong to the shelf (hover shows the arrows)
    }
    float cardW = kMinCard;
    float viewW = 0;   // the page's content width

    float contentW() const {
        int n = 0;
        for (const auto& ch : children())
            if (ch->visible()) ++n;
        return n > 0 ? n * cardW + (n - 1) * kCardGap : 0.f;
    }
    float maxScroll() const { return std::max(0.f, std::round(contentW() - viewW)); }
    float target() const { return offset_.target(); }

    void layout() override {
        const Rect r = rect();
        float x = Shelf::kBleed;
        const float h = std::max(0.f, r.h - Shelf::kPadTop - Shelf::kPadBottom);
        for (auto& ch : children()) {
            if (!ch->visible()) continue;
            ch->setRect({x, Shelf::kPadTop, cardW, h});
            x += cardW + kCardGap;
        }
        offset_.setBounds(0, maxScroll());
    }
    gfx::Point contentOffset() const override { return {-offset_.value(), 0}; }
    gfx::Point settledContentOffset() const override { return {-offset_.target(), 0}; }

    void glideTo(float x) {
        offset_.setBounds(0, maxScroll());
        offset_.glideTo(std::clamp(x, 0.f, maxScroll()));
        invalidate();
    }
    // A page of whole cards, landing on a column.
    void page(int dir) {
        const float step = cardW + kCardGap;
        const int cols = std::max(1, static_cast<int>((viewW + kCardGap) / step));
        glideTo(std::round(offset_.target() / step + static_cast<float>(dir * cols)) * step);
    }

    // Keyboard focus on a card out of view: the least glide that shows it, a column boundary on the side it came in.
    void revealRect(const Rect& r) override {
        if (maxScroll() <= 0) return;
        const float t = offset_.target();
        const float left = t + Shelf::kBleed, right = left + viewW;
        if (r.x >= left - 0.5f && r.right() <= right + 0.5f) return;
        glideTo(r.x < left ? r.x - Shelf::kBleed : r.right() - Shelf::kBleed - viewW);
    }

    // Sideways input only: a horizontal wheel / touchpad swipe, or Shift + wheel. Plain wheel scrolls the page.
    bool onWheel(float dy, float dx, const ui::MouseEvent& e) override {
        if (maxScroll() <= 0) return false;
        float notches = 0;
        if (dx != 0) notches = -dx;
        else if (e.shift && dy != 0) notches = dy;
        else return false;
        offset_.setBounds(0, maxScroll());
        const bool moved = offset_.wheel(notches, 120.f, viewW * 0.9f);
        invalidate();
        return moved;
    }

    void paint(Canvas& c) override {
        const float x = offset_.value();   // advances the glide (and asks for the next frame while it moves)
        const bool moving = offset_.moving();
        if (wasMoving_ && !moving)
            if (auto* w = window()) w->refreshHover();   // cards that slid under a still pointer
        wasMoving_ = moving;
        const Rect r = rect();
        const float m = maxScroll();
        // Fade out into the gutters where there is more (only the gutter: cards inside the page stay whole).
        int layers = 0;
        if (x > 0.5f) {
            c.pushFade(r, {r.x + 2, 0}, {r.x + Shelf::kBleed, 0}, 0.f, 1.f);
            ++layers;
        }
        if (x < m - 0.5f) {
            c.pushFade(r, {r.right() - 2, 0}, {r.right() - Shelf::kBleed, 0}, 0.f, 1.f);
            ++layers;
        }
        paintChildren(c);
        while (layers-- > 0) c.popLayer();
    }

private:
    ui::SmoothScroll offset_;
    bool wasMoving_ = false;
};

// A round arrow at a shelf's end: opaque (it sits on artwork), hairline ring, chevron. Shown while the pointer is over
// the shelf and there is more that way.
class ShelfArrow : public ui::Widget {
public:
    explicit ShelfArrow(int dir) : dir_(dir) { setTooltip(dir < 0 ? tr(L"Önceki") : tr(L"Sonraki")); }
    std::function<void()> onClick;
    bool can = false;   // more cards that way (set by the shelf each frame)
    float shown = 0;    // the shelf's hover

    void paint(Canvas& c) override {
        const float a = can ? shown : 0.f;
        if (a <= 0.01f) return;
        const Rect r = rect();
        const auto& col = colors();
        const float h = hover_.value();
        const gfx::Point ctr{r.cx(), r.cy()};
        const float rad = r.w * 0.5f * (1 - 0.04f * press_.value());
        c.fillCircle(ctr, rad, Color::lerp(col.bgElevated, Color::lerp(col.bgElevated, col.fgPrimary, 0.08f), h).mulAlpha(a));
        c.strokeCircle(ctr, rad - 0.5f, Color::lerp(col.hairStrong, col.fgSecondary, h).mulAlpha(a));
        c.icon(dir_ < 0 ? "chevron-left" : "chevron-right", r.center(16, 16), col.fgPrimary.mulAlpha(a));
    }
    Widget* hitTest(gfx::Point p) override { return can && shown > 0.3f ? Widget::hitTest(p) : nullptr; }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        press_.to(1, 80);
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, 160);
        if (rect().contains(e.pos) && onClick) onClick();
    }
    void onMouseEnter() override {
        hover_.to(1, ui::motion::fast);
        invalidate();
    }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::fast);
        invalidate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }

private:
    int dir_;
    ui::Anim hover_, press_;
};

Shelf::Shelf() {
    hitTestVisible = false;
    track_ = add<ShelfTrack>();
    left_ = add<ShelfArrow>(-1);
    right_ = add<ShelfArrow>(1);
    left_->onClick = [this] { track_->page(-1); };
    right_->onClick = [this] { track_->page(1); };
}

ui::Widget* Shelf::row() { return track_; }

bool Shelf::scrollable() const { return track_->maxScroll() > 0; }

void Shelf::scrollPages(int pages) { track_->page(pages); }

float Shelf::preferredHeight(float width) {
    const float cw = cardWidth(width);
    float h = 0;
    for (const auto& ch : track_->children())
        if (ch->visible()) h = std::max(h, ch->preferredHeight(cw));
    return h > 0 ? kPadTop + std::ceil(h) + kPadBottom : 0.f;
}

void Shelf::layout() {
    const Rect r = rect();
    track_->cardW = cardWidth(r.w);
    track_->viewW = r.w;
    track_->setRect({-kBleed, 0, r.w + 2 * kBleed, r.h});
    track_->layout();   // the card width can change at the same track size (density)
    // The arrows sit on the page's edges, centered on the artwork.
    constexpr float s = 36;
    const float cy = kPadTop + track_->cardW * 0.5f;
    left_->setRect({-s * 0.5f, std::round(cy - s * 0.5f), s, s});
    right_->setRect({r.w - s * 0.5f, std::round(cy - s * 0.5f), s, s});
}

void Shelf::paint(Canvas& c) {
    const float t = track_->target();
    const float m = track_->maxScroll();
    left_->can = t > 0.5f;
    right_->can = t < m - 0.5f;
    left_->shown = right_->shown = hover_.value();
    paintChildren(c);
}

void Shelf::onMouseEnter() {
    hover_.to(1, ui::motion::normal);
    invalidate();
}

void Shelf::onMouseLeave() {
    hover_.to(0, ui::motion::normal);
    invalidate();
}

ui::Widget* Shelf::hitTest(gfx::Point p) {
    if (!visible()) return nullptr;
    const Rect r = rect();
    if (!Rect{r.x - kBleed, r.y, r.w + 2 * kBleed, r.h}.contains(p)) return nullptr;
    const gfx::Point local{p.x - r.x, p.y - r.y};
    for (auto it = children().rbegin(); it != children().rend(); ++it)
        if (Widget* hit = (*it)->hitTest(local)) return hit;
    return nullptr;
}

// =====================================================================================================================
// Skeletons

float ShelfSkeleton::preferredHeight(float width) { return 48 + 16 + cardWidth(width) + 10 + 18 + 4 + 16; }

void ShelfSkeleton::paint(Canvas& c) {
    const Rect r = rect();
    const double now = ui::frame::now();
    c.skeleton({r.x, r.y + 10, 200, 22}, 2, now);
    c.hline(r.x, r.right(), r.y + 44, colors().hairDefault);
    const int n = columns(r.w);
    const float cw = cardWidth(r.w);
    const float y = r.y + 48 + 16;
    for (int i = 0; i < n; ++i) {
        const float x = r.x + i * (cw + kCardGap);
        c.skeleton({x, y, cw, cw}, circles_ ? cw * 0.5f : 2.f, now);
        c.skeleton({x, y + cw + 12, std::round(cw * (0.72f - 0.08f * (i % 3))), 12}, 2, now);
        c.skeleton({x, y + cw + 32, std::round(cw * 0.45f), 10}, 2, now);
    }
    ui::frame::requestNext();
}

float TopSkeleton::preferredHeight(float width) {
    const float hero = width >= 720 ? 216.f : width >= 480 ? 176.f : 148.f;
    return hero + metrics::sectionGap + 2 * quickTileH() + metrics::gridGap;
}

void TopSkeleton::paint(Canvas& c) {
    const Rect r = rect();
    const double now = ui::frame::now();
    const float hero = r.w >= 720 ? 216.f : r.w >= 480 ? 176.f : 148.f;
    c.skeleton({r.x, r.y, r.w, hero}, 2, now);
    const int cols = r.w >= 880 ? 4 : r.w >= 600 ? 3 : 2;
    const float w = std::floor((r.w - metrics::gridGap * (cols - 1)) / cols), h = quickTileH();
    for (int i = 0; i < cols * 2; ++i)
        c.skeleton({r.x + (i % cols) * (w + metrics::gridGap), r.y + hero + metrics::sectionGap + (i / cols) * (h + metrics::gridGap), w, h},
                   2, now);
    ui::frame::requestNext();
}

// =====================================================================================================================

ui::Widget* shelfRow(ui::Column* section) {
    auto* shelf = section->add<Shelf>();
    section->setSpacingBefore(shelf, 16 - Shelf::kPadTop);
    return shelf->row();
}

ui::Widget* addShelf(ui::Column* c, std::wstring title, std::wstring meta, std::wstring link, std::function<void()> onLink) {
    auto* h = c->add<SectionHeader>(std::move(title), std::move(meta), std::move(link));
    if (onLink) h->onLink = std::move(onLink);
    return shelfRow(c);
}

} // namespace st::app::home
