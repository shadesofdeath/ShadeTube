#pragma once
// Small widgets shared by the page implementations.
#include "app/Components.h"
#include "ui/Layout.h"

#include <functional>
#include <string>

namespace st::app {

// Compact row: art 44 + title/subtitle + right mono meta (Yeni çıkanlar, search results, library list). A click arms
// its cover for the page transition; a press and hold previews `previewTarget`.
class ListRow : public ui::Widget, public CoverSource, public PreviewSource {
public:
    ListRow(std::wstring title, std::wstring subtitle, std::wstring meta, std::vector<catalog::Image> images,
            bool circle = false);
    std::function<void()> onClick;
    std::function<void(gfx::Point)> onContext;
    float preferredHeight(float) override { return 64; }
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override { hover_.to(0, ui::motion::microMs(ui::motion::fast)); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Keyboard (focusable): Enter / Space = click, menu key / Shift+F10 = context menu.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
        pagetx::arm(cover());
        const auto click = onClick;   // may navigate away (this row is deleted after the event)
        click();
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if ((e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) || !onContext) return false;
        const Rect r = toWindow(rect());
        const auto context = onContext;
        context({r.x + 56, r.bottom()});
        return true;
    }
    // Song preview (app/Preview): what a press and hold on the row samples; unset = none.
    std::optional<preview::Target> previewTarget;
    std::optional<PreviewHit> previewAt(gfx::Point, bool) override {
        if (!previewTarget) return std::nullopt;
        return PreviewHit{*previewTarget, toWindow(rect())};
    }
    bool coverFor(const std::string& key, SharedCover& out) override;

private:
    Rect artRect() const { return {rect().x + 4, rect().cy() - 22, 44, 44}; }
    SharedCover cover() const { return pagetx::imageCover(*this, artRect(), circle_ ? 22.f : 2.f, images_); }
    gfx::Text title_, subtitle_, meta_;
    std::vector<catalog::Image> images_;
    bool circle_;
    ui::Anim hover_;
};

// Colored editorial tile (mixes / categories / top artists). Its round artwork (if any) flies into the page it opens.
// Hover: the card look of ui::card (rise, shadow, hairline). A press and hold previews `previewTarget`.
class Tile : public ui::Widget, public CoverSource, public PreviewSource {
public:
    Tile(std::wstring label, std::wstring title, Color color, std::vector<catalog::Image> images = {}, bool hero = false);
    std::function<void()> onClick;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override { hover_.to(0, ui::motion::microMs(ui::motion::fast)); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Keyboard (focusable): Enter / Space = click, with the click's press feedback.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
        press_.snap(ui::motion::micro() ? 1.f : 0.f);
        press_.to(0, ui::motion::microMs(160));
        if (!images_.empty()) pagetx::arm(cover());
        const auto click = onClick;
        click();
        return true;
    }
    // Song preview (app/Preview): what a press and hold on the tile samples; unset = none.
    std::optional<preview::Target> previewTarget;
    std::optional<PreviewHit> previewAt(gfx::Point, bool) override {
        if (!previewTarget) return std::nullopt;
        return PreviewHit{*previewTarget, toWindow(rect())};
    }
    bool coverFor(const std::string& key, SharedCover& out) override;

private:
    Rect artRect() const;
    SharedCover cover() const;
    gfx::Text label_, title_;
    gfx::Text labelShort_;   // "02" of "02 · ALBÜM": used when the whole label does not fit next to the artwork
    Color color_;
    std::vector<catalog::Image> images_;
    bool hero_;
    ui::Anim hover_, press_;
};

// Fixed-height spacer / custom painter.
class Spacer : public ui::Widget {
public:
    explicit Spacer(float h) : h_(h) { hitTestVisible = false; }
    float preferredHeight(float) override { return h_; }

private:
    float h_;
};

// ---- Shared page states: empty, error, offline, loading ---------------------------------------------------------------
// Every page uses these (docs/DEVELOPMENT.md "List design"): one look for "nothing here yet", "that failed" and
// "loading", so a page never invents its own.

// Centered state message: a thin outline illustration (a dial of hairline ticks around a ring with the icon), one
// title line, one short line of explanation and at most one action.
//  - Tone::Empty: neutral ink. Tone::Error: a small status.error dot on the ring, the action defaults to a "refresh"
//    icon. Tone::Offline: the offline icon and the warning dot. The tone is guessed from the icon ("offline" /
//    "error" / "warning" = an error state) and can be set explicitly.
//  - Compact: a smaller, left-free inline version (inside a list or a panel: "no matches", an empty queue).
// It fades in and rises 6 DIP when first painted (none without micro-interactions).
class MessagePanel : public ui::Widget {
public:
    enum class Tone { Empty, Error, Offline };
    MessagePanel(std::string icon, std::wstring title, std::wstring body, std::wstring action = {},
                 std::function<void()> onAction = {});
    MessagePanel& setTone(Tone tone);
    MessagePanel& setCompact(bool on);
    MessagePanel& setActionIcon(std::string icon);   // "" = a text-only button
    Tone tone() const { return tone_; }
    float preferredHeight(float width) override;
    void layout() override;
    void paint(Canvas& c) override;

private:
    float illoSize() const { return compact_ ? 56.f : 88.f; }
    float textW(float width) const { return std::min(compact_ ? 360.f : 440.f, std::max(120.f, width - 32)); }
    std::string icon_;
    gfx::Text title_, body_;
    ui::Button* action_ = nullptr;
    Tone tone_ = Tone::Empty;
    bool compact_ = false;
    bool shown_ = false;
    ui::Anim in_{1.f};
};

// The usual states, so pages don't spell them out (they return the panel added to `parent`):
//  emptyState: Tone::Empty, an optional action (no icon unless `actionIcon` is given).
//  errorState: a failed load with "Tekrar dene"; a connectivity failure becomes the offline state ("Çevrimdışısın",
//  the offline icon) and other messages are made readable (friendlyError: session, rate limit, 403 / 404).
MessagePanel* emptyState(ui::Widget* parent, std::string icon, std::wstring title, std::wstring line,
                         std::wstring action = {}, std::function<void()> onAction = {}, std::string actionIcon = {});
MessagePanel* errorState(ui::Widget* parent, const std::wstring& rawMessage, std::function<void()> retry);
// A search inside a page that matched nothing: the compact state with the search glyph (pages that keep one around
// toggle it with setVisible).
MessagePanel* noResultsState(ui::Widget* parent, std::wstring line);
// Raw service errors -> one short sentence in the UI language; `offline` is set for connectivity failures.
std::wstring friendlyError(const std::wstring& raw, bool* offline = nullptr);

// Skeleton placeholders (ui::paintSkeleton: one soft band across the window, static with reduced motion). Bars vary
// in length deterministically so a list of them reads like text, not stripes. None of them takes input.
// Track rows in the track table's geometry (density aware: TrackTable::rowMetrics) with an optional column header.
class SkeletonRows : public ui::Widget {
public:
    explicit SkeletonRows(int rows, bool art = true, bool header = false) : rows_(rows), art_(art), header_(header) {
        hitTestVisible = false;
    }
    float preferredHeight(float) override;
    void paint(Canvas& c) override;

private:
    int rows_;
    bool art_, header_;
};

// A grid of media cards (square, or round for artists): `rows` rows of as many `minCell` columns as fit.
class SkeletonCards : public ui::Widget {
public:
    SkeletonCards(int rows, float minCell = 170, bool round = false) : rows_(rows), minCell_(minCell), round_(round) {
        hitTestVisible = false;
    }
    float preferredHeight(float width) override { return gridHeight(width, rows_, minCell_); }
    void paint(Canvas& c) override { paintGrid(c, this, rect(), rows_, minCell_, round_); }
    static float gridHeight(float width, int rows, float minCell);
    static void paintGrid(Canvas& c, ui::Widget* owner, const Rect& r, int rows, float minCell, bool round);

private:
    int rows_;
    float minCell_;
    bool round_;
};

// A shelf: a section title bar over one row of cards (Home / New releases / Stats sections).
class SkeletonShelf : public ui::Widget {
public:
    explicit SkeletonShelf(float minCell = 170, bool round = false) : minCell_(minCell), round_(round) {
        hitTestVisible = false;
    }
    float preferredHeight(float width) override { return 48 + SkeletonCards::gridHeight(width, 1, minCell_); }
    void paint(Canvas& c) override;

private:
    float minCell_;
    bool round_;
};

// A collection header (the 232 DIP cover, label, title, meta, the action row). The cover slot stays empty while a
// cover flies into the loading page (pagetx::coverIncoming).
class SkeletonHeader : public ui::Widget {
public:
    SkeletonHeader() { hitTestVisible = false; }
    static constexpr float kHeight = 232 + 28 + 56;
    float preferredHeight(float) override { return kHeight; }
    void paint(Canvas& c) override { paintAt(c, this, rect()); }
    static void paintAt(Canvas& c, ui::Widget* owner, const Rect& r);
};

// A page title (the display title and the mono line under it) while the page loads (ScrollPage::showLoading).
class SkeletonTitle : public ui::Widget {
public:
    SkeletonTitle() { hitTestVisible = false; }
    float preferredHeight(float) override { return 88; }
    void paint(Canvas& c) override;
};

// A collection page loading: SkeletonHeader + `rows` track rows (ScrollPage::showSkeleton).
class SkeletonBlock : public ui::Widget {
public:
    explicit SkeletonBlock(int rows) : rows_(rows) { hitTestVisible = false; }
    float preferredHeight(float) override;
    void paint(Canvas& c) override;

private:
    int rows_;
};

// Horizontal row of cards with a fixed number of visible columns (Grid with maxRows = 1).
ui::Grid* addCardRow(ui::Column* col, float minCell, int maxRows = 1);

} // namespace st::app
