#pragma once
// Small widgets shared by the page implementations.
#include "app/Components.h"
#include "ui/Layout.h"

#include <functional>
#include <string>

namespace st::app {

// Compact row: art 44 + title/subtitle + right mono meta (Yeni çıkanlar, search results, library list).
class ListRow : public ui::Widget {
public:
    ListRow(std::wstring title, std::wstring subtitle, std::wstring meta, std::vector<catalog::Image> images,
            bool circle = false);
    std::function<void()> onClick;
    std::function<void(gfx::Point)> onContext;
    float preferredHeight(float) override { return 64; }
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Keyboard (focusable): Enter / Space = click, menu key / Shift+F10 = context menu.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
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

private:
    gfx::Text title_, subtitle_, meta_;
    std::vector<catalog::Image> images_;
    bool circle_;
    ui::Anim hover_;
};

// Colored editorial tile (mixes / categories / top artists).
class Tile : public ui::Widget {
public:
    Tile(std::wstring label, std::wstring title, Color color, std::vector<catalog::Image> images = {}, bool hero = false);
    std::function<void()> onClick;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    // Keyboard (focusable): Enter / Space = click, with the click's press feedback.
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onClick) return false;
        press_.snap(1);
        press_.to(0, 160);
        const auto click = onClick;
        click();
        return true;
    }

private:
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

// Centered message with optional action (empty / error states).
class MessagePanel : public ui::Widget {
public:
    MessagePanel(std::string icon, std::wstring title, std::wstring body, std::wstring action = {},
                 std::function<void()> onAction = {});
    float preferredHeight(float) override { return 320; }
    void layout() override;
    void paint(Canvas& c) override;

private:
    std::string icon_;
    gfx::Text title_, body_;
    ui::Button* action_ = nullptr;
};

// Skeleton placeholder: a hero block + rows.
class SkeletonBlock : public ui::Widget {
public:
    explicit SkeletonBlock(int rows) : rows_(rows) { hitTestVisible = false; }
    float preferredHeight(float) override { return 260.f + rows_ * 56.f; }
    void paint(Canvas& c) override;

private:
    int rows_;
};

// Horizontal row of cards with a fixed number of visible columns (Grid with maxRows = 1).
ui::Grid* addCardRow(ui::Column* col, float minCell, int maxRows = 1);

} // namespace st::app
