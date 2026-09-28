#pragma once
// Building blocks of the Ayarlar page, shared with the feature files that contribute their own rows
// (see the build*() hooks below; SettingsPage owns the section headers and the order).
// Every text here is shown as given: callers pass it translated (tr(L"Ses kalitesi")).
#include "app/AppContext.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "core/Settings.h"
#include "gfx/Theme.h"
#include "ui/Controls.h"
#include "ui/Layout.h"

#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace st::app {

inline void openPath(const std::wstring& path) { ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }

// label + description on the left, a control on the right, hairline below.
class SettingRow : public ui::Widget {
public:
    SettingRow(std::wstring title, std::wstring desc) : title_(std::move(title), gfx::type::body), desc_(std::move(desc), gfx::type::secondary) {
        hitTestVisible = false;
        gfx::TextOptions wrap;
        wrap.wrap = true;
        desc_.setOptions(wrap);
    }
    // `w` is the width the Turkish label was designed for; a control with a natural width (buttons, segmented
    // controls) grows past it when a translation is longer.
    template <class T, class... A>
    T* control(float w, A&&... args) {
        controlW_ = w;
        auto* c = add<T>(std::forward<A>(args)...);
        control_ = c;
        if constexpr (requires { c->naturalWidth(); }) natural_ = [c] { return static_cast<float>(c->naturalWidth()); };
        return c;
    }
    float controlWidth() const { return natural_ ? std::max(controlW_, std::ceil(natural_())) : controlW_; }
    float preferredHeight(float w) override { return std::max(64.f, 22 + 20 + desc_.measure(w - controlWidth() - 48).h + 18); }
    void layout() override {
        if (!control_) return;
        const Rect r = rect();
        const float h = dynamic_cast<ui::Toggle*>(control_) ? 22.f : 40.f;
        const float cw = controlWidth();
        control_->setRect({r.w - cw, r.h * 0.5f - h * 0.5f, cw, h});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = gfx::colors();
        const float tw = r.w - controlWidth() - 48;
        c.text(title_, {r.x, r.y + 18, tw, 20}, col.fgPrimary);
        c.text(desc_, {r.x, r.y + 40, tw, 60}, col.fgSecondary);
        c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
        paintChildren(c);
    }

private:
    gfx::Text title_, desc_;
    float controlW_ = 0;
    ui::Widget* control_ = nullptr;
    std::function<float()> natural_;
};

class Segmented : public ui::Widget {
public:
    Segmented(std::vector<std::wstring> items, int selected) : selected_(selected) {
        focusable = true;
        for (auto& s : items) texts_.emplace_back(std::move(s), gfx::type::caption.withWeight(600));
    }
    std::function<void(int)> onChange;
    // Keyboard (Tab stop): Left/Right step through the segments, Home/End jump to the ends, Enter / Space pick the
    // next one (wrapping). Each change applies immediately, like a click.
    bool activatable() const override { return true; }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt) return false;
        const int n = static_cast<int>(texts_.size());
        switch (e.vk) {
        case VK_LEFT: select(std::max(0, selected_ - 1)); return true;
        case VK_RIGHT: select(std::min(n - 1, selected_ + 1)); return true;
        case VK_HOME: select(0); return true;
        case VK_END: select(n - 1); return true;
        default: return false;
        }
    }
    bool onActivate() override {
        if (!texts_.empty()) select((selected_ + 1) % static_cast<int>(texts_.size()));
        return true;
    }
    Rect focusRect() const override { return rect().center(naturalWidth(), 32); }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Pill; }
    void select(int i) {
        if (i == selected_ || i < 0 || i >= static_cast<int>(texts_.size())) return;
        selected_ = i;
        if (onChange) onChange(i);
        invalidate();
    }
    float naturalWidth() const {
        float w = 4;
        for (auto& t : texts_) w += std::ceil(t.measure().w) + 24;
        return w;
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        const Rect r = rect().center(naturalWidth(), 32);
        float x = r.x + 2;
        for (int i = 0; i < static_cast<int>(texts_.size()); ++i) {
            const float w = std::ceil(texts_[i].measure().w) + 24;
            if (e.pos.x >= x && e.pos.x < x + w) {
                selected_ = i;
                if (onChange) onChange(i);
                invalidate();
                return true;
            }
            x += w;
        }
        return false;
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    void paint(Canvas& c) override {
        const Rect r = rect().center(naturalWidth(), 32);
        const auto& col = gfx::colors();
        c.strokePill(r, col.hairDefault);
        float x = r.x + 2;
        for (int i = 0; i < static_cast<int>(texts_.size()); ++i) {
            const float w = std::ceil(texts_[i].measure().w) + 24;
            const Rect item{x, r.y + 2, w, r.h - 4};
            if (i == selected_) c.fillPill(item, col.fgPrimary);
            c.text(texts_[i], item.inset(12, 0), i == selected_ ? col.fgInverse : col.fgSecondary, gfx::VAlign::Center);
            x += w;
        }
    }

private:
    mutable std::vector<gfx::Text> texts_;   // measure() caches layouts
    int selected_;
};

// Section label ("OYNATMA") with the page's spacing. Returns the label (deep-link target).
inline ui::Label* settingsSection(ui::Column* c, const wchar_t* label) {
    c->add<Spacer>(28.f);
    auto* l = c->add<ui::Label>(label, gfx::type::monoLabel, ui::Tone::Tertiary);
    l->setVAlign(gfx::VAlign::Top);
    c->add<Spacer>(4.f);
    return l;
}

// Title + description + a Toggle. `apply` runs on change, then Settings is marked dirty (saved by App).
inline ui::Toggle* settingsToggle(ui::Column* c, const std::wstring& title, const std::wstring& desc, bool value,
                                  std::function<void(bool)> apply) {
    auto* row = c->add<SettingRow>(title, desc);
    auto* t = row->control<ui::Toggle>(40.f);
    t->setOn(value, false);
    t->onChange = [apply = std::move(apply)](bool v) {
        apply(v);
        Settings::get().markDirty();
    };
    return t;
}

// ---- Feature rows (each defined in its feature's file) ------------------------------------------------
// `rebuild` re-runs the whole page build (posted, safe to call from a click handler that deletes the row).
void buildEndlessPlaybackRows(ui::Column* c, const std::function<void()>& rebuild);   // OYNATMA   (app/PlaybackSettings.cpp)
void buildAltSourceRows(ui::Column* c, const std::function<void()>& rebuild);         // OYNATMA   (app/AltSourceSettings.cpp)
void buildBlacklistSection(ui::Column* c, const std::function<void()>& rebuild);      // KARA LİSTE (app/PlaybackSettings.cpp)
void buildLocalFilesSection(ui::Column* c, const std::function<void()>& rebuild);     // YEREL MÜZİK (app/LocalFilesPage.cpp)
void buildAboutSection(ui::Column* c, const std::function<void()>& rebuild);          // HAKKINDA  (app/AboutSettings.cpp)

} // namespace st::app
