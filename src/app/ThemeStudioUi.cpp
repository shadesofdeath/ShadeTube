// Theme studio widgets (Ayarlar › Görünüm): the theme gallery, the accent swatches, the color picker, the font picker,
// the theme editor and the rows of the Görünüm card. Logic in ThemeStudio.cpp.
#include "app/ThemeStudio.h"

#include "app/App.h"
#include "app/SettingsWidgets.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Text.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>
#include <unordered_map>

namespace st::app {

namespace {

using gfx::accent;
using gfx::colors;
using gfx::ThemeKey;
using gfx::ThemeSpec;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

// ---- HSV -----------------------------------------------------------------------------------------------------------

struct Hsv {
    float h = 0, s = 0, v = 0;   // h 0..360, s / v 0..1
};

Hsv toHsv(Color c) {
    const float mx = std::max({c.r, c.g, c.b}), mn = std::min({c.r, c.g, c.b}), d = mx - mn;
    Hsv o;
    o.v = mx;
    o.s = mx > 1e-6f ? d / mx : 0.f;
    if (d > 1e-6f) {
        if (mx == c.r) o.h = 60.f * std::fmod((c.g - c.b) / d + 6.f, 6.f);
        else if (mx == c.g) o.h = 60.f * ((c.b - c.r) / d + 2.f);
        else o.h = 60.f * ((c.r - c.g) / d + 4.f);
    }
    return o;
}

Color fromHsv(Hsv in) {
    const float h = std::fmod(std::max(0.f, in.h), 360.f) / 60.f, s = std::clamp(in.s, 0.f, 1.f), v = std::clamp(in.v, 0.f, 1.f);
    const float c = v * s, x = c * (1 - std::abs(std::fmod(h, 2.f) - 1)), m = v - c;
    float r = 0, g = 0, b = 0;
    switch (static_cast<int>(h)) {
    case 0: r = c, g = x; break;
    case 1: r = x, g = c; break;
    case 2: g = c, b = x; break;
    case 3: g = x, b = c; break;
    case 4: r = x, b = c; break;
    default: r = c, b = x; break;
    }
    return {r + m, g + m, b + m, 1.f};
}

std::wstring hexText(Color c) { return toWide(gfx::colorString(c.withAlpha(1))); }

// "#RRGGBB", "RRGGBB" or "#RGB" as typed.
std::optional<Color> parseHexInput(std::wstring s) {
    std::erase_if(s, [](wchar_t ch) { return ch == L' '; });
    if (!s.empty() && s[0] == L'#') s.erase(0, 1);
    if (s.size() == 3) s = {s[0], s[0], s[1], s[1], s[2], s[2]};
    if (s.size() != 6) return std::nullopt;
    return gfx::tryParseColor("#" + toUtf8(s));
}

std::wstring ratioText(float r) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", r);
    std::wstring n = b;
    std::replace(n.begin(), n.end(), L'.', i18n::decimalSeparator());
    return n;
}

// The accent a theme shows with the current accent setting: the fixed color, the theme's own, or the cover's.
Color accentFor(const ThemeSpec& t) {
    Color base = t.accent;
    const auto& s = Settings::get();
    if (s.accentMode == AccentMode::Fixed) base = themes::fixedAccent();
    else if (s.accentMode == AccentMode::Dynamic)
        if (const auto r = gfx::Theme::get().requestedAccent()) base = *r;
    return gfx::Theme::adaptAccent(base, t.palette.bgBase, t.light);
}

// A tiny ShadeTube in a theme's colors: sidebar with a selected item, a title, a card with two text lines and the
// accent play button, the player bar with its progress line.
void paintMiniMock(Canvas& c, const Rect& m, const ThemeSpec& t) {
    const auto& p = t.palette;
    const Color a = accentFor(t);
    const float r = std::min(gfx::radii().surface, 6.f);
    const bool layer = r > 0;   // a rounded clip is a layer, a square one an axis clip: pop the same kind
    if (layer) c.pushRoundedClip(m, r);
    else c.pushClip(m);
    c.fillRect(m, p.bgBase);
    const float sw = std::round(m.w * 0.27f), barH = 15;
    c.vline(m.x + sw, m.y, m.bottom() - barH, p.hairDefault);
    // Sidebar: the logo dot, a selected nav item, two more, a playlist tile.
    c.fillCircle({m.x + 8, m.y + 8}, 2.5f, a);
    const float nx = m.x + 4, nw = sw - 8;
    c.fillRounded({nx, m.y + 16, nw, 9}, 4.5f, p.overlaySelected);
    c.fillRounded({nx + 4, m.y + 19.5f, nw * 0.55f, 2}, 1, p.fgPrimary);
    c.fillRounded({nx + 4, m.y + 31, nw * 0.45f, 2}, 1, p.fgSecondary);
    c.fillRounded({nx + 4, m.y + 40, nw * 0.5f, 2}, 1, p.fgSecondary);
    c.fillRounded({nx + 4, m.y + 50, 7, 7}, std::min(r, 2.f), p.bgOverlay);
    // Content: title, a card.
    const float cx = m.x + sw + 8, cw = m.right() - 8 - cx;
    c.fillRounded({cx, m.y + 9, cw * 0.5f, 3}, 1.5f, p.fgPrimary);
    const Rect card{cx, m.y + 19, cw, 30};
    c.fillRounded(card, std::min(r, 3.f), p.bgRaised);
    c.strokeRounded(card.inset(0.5f), std::min(r, 3.f), p.hairDefault);
    c.fillRounded({card.x + 6, card.y + 9, card.w * 0.45f, 2.5f}, 1, p.fgPrimary);
    c.fillRounded({card.x + 6, card.y + 17, card.w * 0.3f, 2}, 1, p.fgSecondary);
    c.fillCircle({card.right() - 11, card.cy()}, 6, a);
    c.fillRounded({cx, card.bottom() + 6, cw * 0.62f, 2}, 1, p.fgTertiary);
    // Player bar.
    const float by = m.bottom() - barH;
    c.hline(m.x, m.right(), by, p.hairSubtle);
    c.strokeCircle({m.cx(), by + barH * 0.5f}, 4, p.fgPrimary, 1);
    c.fillRect({m.x + 8, m.bottom() - 2.5f, m.w - 16, 1}, p.hairStrong);
    c.fillRect({m.x + 8, m.bottom() - 2.5f, (m.w - 16) * 0.42f, 1}, a);
    if (layer) c.popLayer();
    else c.popClip();
}

// ---- Color picker ----------------------------------------------------------------------------------------------------

// Saturation / value area, hue strip and a hex field. Every change (drag, arrow keys, typing a valid hex) calls
// onChange right away (the callers preview live).
class ColorPicker : public ui::Widget {
public:
    explicit ColorPicker(Color initial) : hsv_(toHsv(initial)) {
        hitTestVisible = false;
        sv_ = add<SvArea>(this);
        hue_ = add<HueBar>(this);
        hex_ = add<ui::TextBox>(ui::TextBox::Look::Field, L"#RRGGBB");
        hex_->setText(hexText(initial));
        hex_->onChange = [this](const std::wstring& text) {
            if (const auto c = parseHexInput(text)) {
                hsv_ = toHsv(*c);
                fromText_ = true;
                changed();
                fromText_ = false;
            }
        };
        hex_->onSubmit = [this](const std::wstring&) { hex_->setText(hexText(color())); };
    }
    std::function<void(Color)> onChange;
    Color color() const { return fromHsv(hsv_); }
    void setColor(Color c) {
        hsv_ = toHsv(c);
        hex_->setText(hexText(c));
        invalidate();
    }
    static constexpr float kSvH = 148, kHueH = 12;
    float preferredHeight(float) override { return kSvH + 14 + kHueH + 14 + 44; }
    void layout() override {
        const Rect r = rect();
        sv_->setRect({r.x, r.y, r.w, kSvH});
        hue_->setRect({r.x, r.y + kSvH + 14, r.w, kHueH});
        hex_->setRect({r.x, r.y + kSvH + 14 + kHueH + 14, std::min(140.f, r.w * 0.5f), 44});
    }
    void paint(Canvas& c) override {
        paintChildren(c);
        // Preview chip next to the hex field: the color on the theme's surface, and how it reads as text.
        const Rect f = hex_->rect();
        const Rect chip{f.right() + 12, f.y + 6, 32, 32};
        c.fillRounded(chip, std::min(gfx::radii().surface, 6.f), color());
        c.strokeRounded(chip, std::min(gfx::radii().surface, 6.f), colors().hairStrong);
        const float ratio = gfx::contrastRatio(color(), colors().bgRaised);
        c.text(i18n::format(tr(L"Karşıtlık {}:1"), {ratioText(ratio)}), type::caption,
               {chip.right() + 10, f.y, rect().right() - chip.right() - 10, f.h}, colors().fgSecondary,
               gfx::TextAlign::Leading, gfx::VAlign::Center);
    }

private:
    void changed() {
        if (!fromText_) hex_->setText(hexText(color()));
        invalidate();
        if (onChange) onChange(color());
    }

    class SvArea : public ui::Widget {
    public:
        explicit SvArea(ColorPicker* p) : p_(p) { focusable = true; }
        bool activatable() const override { return true; }
        LPCWSTR cursor() const override { return IDC_CROSS; }
        bool onMouseDown(const ui::MouseEvent& e) override {
            if (e.button != ui::MouseButton::Left) return false;
            pick(e.pos);
            return true;
        }
        void onMouseMove(const ui::MouseEvent& e) override {
            if (pressed()) pick(e.pos);
        }
        bool onKeyDown(const ui::KeyEvent& e) override {
            const float step = e.shift ? 0.1f : 0.01f;
            switch (e.vk) {
            case VK_LEFT: p_->hsv_.s = std::max(0.f, p_->hsv_.s - step); break;
            case VK_RIGHT: p_->hsv_.s = std::min(1.f, p_->hsv_.s + step); break;
            case VK_UP: p_->hsv_.v = std::min(1.f, p_->hsv_.v + step); break;
            case VK_DOWN: p_->hsv_.v = std::max(0.f, p_->hsv_.v - step); break;
            default: return false;
            }
            p_->changed();
            return true;
        }
        Rect focusRect() const override { return rect(); }
        void paint(Canvas& c) override {
            const Rect r = rect();
            const float rad = std::min(gfx::radii().surface, 6.f);
            if (rad > 0) c.pushRoundedClip(r, rad);
            else c.pushClip(r);
            c.fillRect(r, fromHsv({p_->hsv_.h, 1, 1}));
            c.fillLinearGradient(r, {r.x, r.y}, {r.right(), r.y}, Color{1, 1, 1, 1}, Color{1, 1, 1, 0});
            c.fillLinearGradient(r, {r.x, r.y}, {r.x, r.bottom()}, Color{0, 0, 0, 0}, Color{0, 0, 0, 1});
            if (rad > 0) c.popLayer();
            else c.popClip();
            c.strokeRounded(r, rad, colors().hairDefault);
            const gfx::Point h{r.x + p_->hsv_.s * r.w, r.y + (1 - p_->hsv_.v) * r.h};
            c.pushClip(r.inset(-8));
            c.fillCircle(h, 6, p_->color());
            c.strokeCircle(h, 7, Color{0, 0, 0, 0.35f}, 1);
            c.strokeCircle(h, 6, Color{1, 1, 1, 1}, 2);
            c.popClip();
        }

    private:
        void pick(gfx::Point pos) {
            const Rect r = rect();
            p_->hsv_.s = std::clamp((pos.x - r.x) / std::max(1.f, r.w), 0.f, 1.f);
            p_->hsv_.v = 1 - std::clamp((pos.y - r.y) / std::max(1.f, r.h), 0.f, 1.f);
            p_->changed();
        }
        ColorPicker* p_;
    };

    class HueBar : public ui::Widget {
    public:
        explicit HueBar(ColorPicker* p) : p_(p) { focusable = true; }
        bool activatable() const override { return true; }
        LPCWSTR cursor() const override { return IDC_HAND; }
        ui::FocusShape focusShape() const override { return ui::FocusShape::Pill; }
        Widget* hitTest(gfx::Point pos) override {
            const Rect r = rect();
            return visible() && Rect{r.x - 6, r.y - 8, r.w + 12, r.h + 16}.contains(pos) ? this : nullptr;
        }
        bool onMouseDown(const ui::MouseEvent& e) override {
            if (e.button != ui::MouseButton::Left) return false;
            pick(e.pos.x);
            return true;
        }
        void onMouseMove(const ui::MouseEvent& e) override {
            if (pressed()) pick(e.pos.x);
        }
        bool onKeyDown(const ui::KeyEvent& e) override {
            const float step = e.shift ? 10.f : 1.f;
            if (e.vk == VK_LEFT) p_->hsv_.h = std::fmod(p_->hsv_.h - step + 360.f, 360.f);
            else if (e.vk == VK_RIGHT) p_->hsv_.h = std::fmod(p_->hsv_.h + step, 360.f);
            else return false;
            p_->changed();
            return true;
        }
        void paint(Canvas& c) override {
            const Rect r = rect();
            c.pushRoundedClip(r, r.h * 0.5f);
            const float seg = r.w / 6.f;
            for (int i = 0; i < 6; ++i) {
                const Rect s{r.x + seg * static_cast<float>(i), r.y, seg + 0.75f, r.h};
                c.fillLinearGradient(s, {s.x, s.y}, {s.x + seg, s.y}, fromHsv({60.f * static_cast<float>(i), 1, 1}),
                                     fromHsv({60.f * static_cast<float>(i + 1), 1, 1}));
            }
            c.popLayer();
            const float x = r.x + p_->hsv_.h / 360.f * r.w;
            const Rect knob{x - 3, r.y - 3, 6, r.h + 6};
            c.fillPill(knob, Color{1, 1, 1, 1});
            c.strokePill(knob.inset(-0.5f), Color{0, 0, 0, 0.35f});
        }

    private:
        void pick(float x) {
            const Rect r = rect();
            p_->hsv_.h = std::clamp((x - r.x) / std::max(1.f, r.w), 0.f, 0.9999f) * 360.f;
            if (p_->hsv_.s < 0.02f) {   // grey: picking a hue colors it
                p_->hsv_.s = 1;
                p_->hsv_.v = std::max(p_->hsv_.v, 0.5f);
            }
            p_->changed();
        }
        ColorPicker* p_;
    };

    Hsv hsv_;
    SvArea* sv_ = nullptr;
    HueBar* hue_ = nullptr;
    ui::TextBox* hex_ = nullptr;
    bool fromText_ = false;
};

// Suggested accents (Volt, Kor, Buz, Fuşya, Kehribar, Nane, Lavanta, Gül).
constexpr uint32_t kSuggested[] = {0xDDFF47, 0xFF5C3A, 0x7CE0FF, 0xFF4FA3, 0xFFB547, 0x5EE6B0, 0xA78BFA, 0xF4A4B8};

// A row of color circles; Left / Right move the keyboard cursor, Enter / Space pick.
class SwatchRow : public ui::Widget {
public:
    SwatchRow(std::vector<Color> colors, float size) : colors_(std::move(colors)), size_(size) { focusable = true; }
    std::function<void(Color)> onPick;
    std::function<bool(Color)> isSelected;
    bool activatable() const override { return true; }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt || (e.vk != VK_LEFT && e.vk != VK_RIGHT)) return false;
        const int n = static_cast<int>(colors_.size());
        cursor_ = (cursor_ + (e.vk == VK_RIGHT ? 1 : n - 1)) % n;
        invalidate();
        return true;
    }
    bool onActivate() override {
        if (onPick && !colors_.empty()) onPick(colors_[static_cast<size_t>(cursor_)]);
        invalidate();
        return true;
    }
    Rect focusRect() const override { return swatch(static_cast<size_t>(cursor_)); }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Circle; }
    LPCWSTR cursor() const override { return IDC_HAND; }
    float preferredHeight(float) override { return size_ + 12; }
    float naturalWidth() const { return static_cast<float>(colors_.size()) * (size_ + 10); }
    bool onMouseDown(const ui::MouseEvent& e) override {
        for (size_t i = 0; i < colors_.size(); ++i)
            if (swatch(i).inset(-3).contains(e.pos)) {
                cursor_ = static_cast<int>(i);
                if (onPick) onPick(colors_[i]);
                invalidate();
                return true;
            }
        return false;
    }
    void paint(Canvas& c) override {
        for (size_t i = 0; i < colors_.size(); ++i) {
            const Rect s = swatch(i);
            c.fillCircle({s.cx(), s.cy()}, size_ * 0.5f - 3, gfx::Theme::adaptAccent(colors_[i], gfx::isLightTheme()));
            if (isSelected && isSelected(colors_[i])) c.strokeCircle({s.cx(), s.cy()}, size_ * 0.5f, colors().fgPrimary, 1.5f);
        }
    }

private:
    Rect swatch(size_t i) const {
        return {rect().x + static_cast<float>(i) * (size_ + 10), rect().cy() - size_ * 0.5f, size_, size_};
    }
    std::vector<Color> colors_;
    float size_;
    int cursor_ = 0;
};

std::vector<Color> suggestedColors(size_t n) {
    std::vector<Color> v;
    for (size_t i = 0; i < std::min(n, std::size(kSuggested)); ++i) v.push_back(Color::rgb(kSuggested[i]));
    return v;
}

bool sameRgb(Color a, Color b) { return gfx::colorString(a.withAlpha(1)) == gfx::colorString(b.withAlpha(1)); }

// ---- Settings row: the fixed accent ----------------------------------------------------------------------------------

// Five suggestions and a sixth "own color" swatch (a hue ring with + or the color mixed in the picker). Picking one
// makes the accent source Sabit.
class AccentSwatches : public ui::Widget {
public:
    AccentSwatches() { focusable = true; }
    bool activatable() const override { return true; }
    LPCWSTR cursor() const override { return IDC_HAND; }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Circle; }
    Rect focusRect() const override { return swatch(cursor_); }
    float naturalWidth() const { return 6 * kStep; }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.ctrl || e.alt || (e.vk != VK_LEFT && e.vk != VK_RIGHT)) return false;
        cursor_ = (cursor_ + (e.vk == VK_RIGHT ? 1 : 5)) % 6;
        invalidate();
        return true;
    }
    bool onActivate() override {
        pick(cursor_);
        return true;
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        for (int i = 0; i < 6; ++i)
            if (swatch(i).inset(-3).contains(e.pos)) {
                cursor_ = i;
                pick(i);
                return true;
            }
        return false;
    }
    std::wstring tooltip() const override { return hot_ == 5 ? std::wstring(tr(L"Kendi rengini karıştır")) : std::wstring{}; }
    void onMouseMove(const ui::MouseEvent& e) override {
        int hot = -1;
        for (int i = 0; i < 6; ++i)
            if (swatch(i).contains(e.pos)) hot = i;
        hot_ = hot;
    }
    void onMouseLeave() override { hot_ = -1; }
    void paint(Canvas& c) override {
        const auto& s = Settings::get();
        const bool fixed = s.accentMode == AccentMode::Fixed;
        const Color cur = themes::fixedAccent();
        bool known = false;
        for (int i = 0; i < 5; ++i) {
            const Rect r = swatch(i);
            const Color col = Color::rgb(kSuggested[i]);
            c.fillCircle({r.cx(), r.cy()}, 12, gfx::Theme::adaptAccent(col, gfx::isLightTheme()));
            if (sameRgb(col, cur)) {
                known = true;
                if (fixed) c.strokeCircle({r.cx(), r.cy()}, 17, colors().fgPrimary, 1.5f);
            }
        }
        // Own color: a thin hue ring; inside the mixed color (when it is not a suggestion) or a +.
        const Rect r = swatch(5);
        const gfx::Point ctr{r.cx(), r.cy()};
        for (int i = 0; i < 24; ++i)
            c.arc(ctr, 11, static_cast<float>(i) * 15.f, 16.f, fromHsv({static_cast<float>(i) * 15.f, 0.75f, 1}), 2.f);
        if (!known) {
            c.fillCircle(ctr, 8, gfx::Theme::adaptAccent(cur, gfx::isLightTheme()));
            if (fixed) c.strokeCircle(ctr, 17, colors().fgPrimary, 1.5f);
        } else {
            c.icon("plus", r.center(14, 14), colors().fgSecondary);
        }
    }

private:
    static constexpr float kStep = 40;
    Rect swatch(int i) const { return {rect().right() - (6 - static_cast<float>(i)) * kStep, rect().cy() - 17, 34, 34}; }
    void pick(int i) {
        if (i == 5) {
            Dispatcher::post([] { themes::openAccentPicker(); });
            return;
        }
        themes::setFixedAccent(Color::rgb(kSuggested[i]));
        invalidate();
        themes::rowsChanged();   // the source segment shows Sabit
    }
    int cursor_ = 0, hot_ = -1;
};

// ---- Theme gallery -----------------------------------------------------------------------------------------------------

// Two groups (dark, light) of preview tiles: every preset and custom theme of that kind, plus a "Kopyala ve düzenle"
// tile. Click = use it (live), right click / Shift+F10 = more (edit, export, delete), arrows move the keyboard cursor.
class ThemeGallery : public ui::Widget, public SettingsSearchable {
public:
    ThemeGallery() { focusable = true; }

    std::wstring searchText() const override {
        std::wstring s = std::wstring(tr(L"Hazır temalar")) + L"\n" + tr(L"Kopyala ve düzenle") + L"\n" + tr(L"Tema");
        for (const auto& t : themes::store().presets()) s += L"\n" + themes::displayName(t);
        for (const auto& t : themes::store().custom()) s += L"\n" + themes::displayName(t);
        return s;
    }
    float preferredHeight(float w) override {
        rebuild();
        return measure(w);
    }
    void layout() override {
        rebuild();
        measure(rect().w);
    }
    bool activatable() const override { return true; }
    LPCWSTR cursor() const override { return hot_ >= 0 ? IDC_HAND : IDC_ARROW; }
    Rect focusRect() const override {
        return cursor_ >= 0 && cursor_ < static_cast<int>(tiles_.size()) ? tiles_[static_cast<size_t>(cursor_)].rect.inset(-1)
                                                                         : rect();
    }
    std::wstring tooltip() const override {
        if (hot_ < 0 || hot_ >= static_cast<int>(tiles_.size())) return {};
        const Tile& t = tiles_[static_cast<size_t>(hot_)];
        if (t.add) return tr(L"Seçili temayı kopyalayıp renklerini değiştir");
        return {};
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        const int n = static_cast<int>(tiles_.size());
        if (n == 0) return false;
        if (e.vk == VK_APPS || (e.vk == VK_F10 && e.shift)) {
            const Rect r = toWindow(tiles_[static_cast<size_t>(cursor_)].rect);
            menu(cursor_, {r.x + 12, r.bottom() - 8});
            return true;
        }
        if (e.ctrl || e.alt) return false;
        int next = cursor_;
        switch (e.vk) {
        case VK_LEFT: next = std::max(0, cursor_ - 1); break;
        case VK_RIGHT: next = std::min(n - 1, cursor_ + 1); break;
        case VK_UP: next = nearest(cursor_, -1); break;
        case VK_DOWN: next = nearest(cursor_, 1); break;
        case VK_HOME: next = 0; break;
        case VK_END: next = n - 1; break;
        default: return false;
        }
        if (next != cursor_) {
            cursor_ = next;
            scrollIntoView(focusRect());
            invalidate();
        }
        return true;
    }
    bool onActivate() override {
        activate(cursor_);
        return true;
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        const int i = tileAt(e.pos);
        if (i < 0) return false;
        cursor_ = i;
        if (e.button == ui::MouseButton::Right) {
            menu(i, e.windowPos);
            return true;
        }
        if (e.button != ui::MouseButton::Left) return true;
        const Tile& t = tiles_[static_cast<size_t>(i)];
        if (e.clicks == 2 && !t.add && ThemeStore::isCustom(t.id)) {
            const std::string id = t.id;
            Dispatcher::post([id] { themes::openEditor(id, false); });
            return true;
        }
        activate(i);
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const int hot = tileAt(e.pos);
        if (hot != hot_) {
            hot_ = hot;
            invalidate();
        }
    }
    void onMouseLeave() override {
        hot_ = -1;
        invalidate();
    }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect r = rect();
        c.text(toUpperTr(tr(L"Koyu")), type::monoLabel, {r.x, darkLabelY_, r.w, kLabelH}, col.fgTertiary);
        c.text(toUpperTr(tr(L"Açık")), type::monoLabel, {r.x, lightLabelY_, r.w, kLabelH}, col.fgTertiary);
        const std::string& onScreen = gfx::Theme::get().themeId();
        const std::string darkId = themes::chosen(false), lightId = themes::chosen(true);
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const Tile& t = tiles_[i];
            const bool hot = static_cast<int>(i) == hot_;
            const Rect mock{t.rect.x, t.rect.y, t.rect.w, kMockH};
            const float rad = std::min(gfx::radii().surface, 6.f);
            if (t.add) {
                if (hot) c.fillRounded(mock, rad, col.overlayHover);
                c.strokeRounded(mock.inset(0.5f), rad, hot ? col.hairStrong : col.hairDefault);
                c.icon("plus", mock.center(18, 18), hot ? col.fgPrimary : col.fgSecondary);
                c.text(tr(L"Kopyala ve düzenle"), type::caption, {t.rect.x, mock.bottom() + 8, t.rect.w, 18},
                       hot ? col.fgPrimary : col.fgSecondary);
                continue;
            }
            const ThemeSpec* spec = themes::store().find(t.id);
            if (!spec) continue;
            paintMiniMock(c, mock, *spec);
            const bool selected = t.id == (spec->light ? lightId : darkId);
            c.strokeRounded(mock.inset(0.5f), rad, hot ? col.hairControl : col.hairDefault);
            if (selected) c.strokeRounded(mock.inset(-3.5f), rad > 0 ? rad + 3.5f : 0.f, col.fgPrimary, 1.5f);
            // Name; a dot marks the theme on screen now (both groups show a choice in Sistem mode).
            float nx = t.rect.x;
            if (t.id == onScreen) {
                c.fillCircle({nx + 3, mock.bottom() + 17}, 3, accent().base);
                nx += 11;
            }
            const TextStyle st = selected ? type::caption.withWeight(600) : type::caption;
            c.text(themes::displayName(*spec), st, {nx, mock.bottom() + 8, t.rect.right() - nx, 18},
                   selected ? col.fgPrimary : col.fgSecondary);
        }
    }

private:
    using TextStyle = gfx::TextStyle;
    struct Tile {
        std::string id;   // "" for an add tile
        bool light = false;
        bool add = false;
        Rect rect{};
    };
    static constexpr float kMinTile = 124, kMaxTile = 156, kMockH = 80, kNameH = 30, kGap = 14, kLabelH = 18;

    // The tiles from the store (it changes with imports / saves / deletes).
    void rebuild() {
        std::vector<Tile> next;
        for (const bool light : {false, true}) {
            for (const auto& t : themes::store().presets())
                if (t.light == light) next.push_back({t.id, light, false, {}});
            for (const auto& t : themes::store().custom())
                if (t.light == light) next.push_back({t.id, light, false, {}});
            next.push_back({{}, light, true, {}});
        }
        if (next.size() != tiles_.size() ||
            !std::equal(next.begin(), next.end(), tiles_.begin(), [](const Tile& a, const Tile& b) { return a.id == b.id && a.light == b.light; })) {
            tiles_ = std::move(next);
            cursor_ = std::clamp(cursor_, 0, static_cast<int>(tiles_.size()) - 1);
        }
    }

    // Places the tiles for width w (in the parent's coordinates, like rect()) and returns the height.
    float measure(float w) {
        const Rect r = rect();
        const int cols = std::max(1, static_cast<int>((w + kGap) / (kMinTile + kGap)));
        const float tw = std::min(kMaxTile, (w - kGap * static_cast<float>(cols - 1)) / static_cast<float>(cols));
        float y = r.y + 12;
        for (const bool light : {false, true}) {
            (light ? lightLabelY_ : darkLabelY_) = y;
            y += kLabelH + 8;
            int i = 0;
            float rowY = y;
            for (auto& t : tiles_) {
                if (t.light != light) continue;
                const int cx = i % cols, cy = i / cols;
                rowY = y + static_cast<float>(cy) * (kMockH + kNameH + kGap);
                t.rect = {r.x + 3 + static_cast<float>(cx) * (tw + kGap), rowY, tw - 6, kMockH + kNameH};
                ++i;
            }
            y = rowY + kMockH + kNameH + 16;
        }
        return y - r.y;
    }

    int tileAt(gfx::Point p) const {
        for (size_t i = 0; i < tiles_.size(); ++i)
            if (tiles_[i].rect.inset(-4).contains(p)) return static_cast<int>(i);
        return -1;
    }

    // The tile above / below `from` (closest in x), or `from` at the top / bottom.
    int nearest(int from, int dir) const {
        const Rect a = tiles_[static_cast<size_t>(from)].rect;
        int best = from;
        float bestRow = dir > 0 ? 1e9f : -1e9f, bestDx = 1e9f;
        for (size_t i = 0; i < tiles_.size(); ++i) {
            const Rect b = tiles_[i].rect;
            if (dir > 0 ? b.y <= a.y + 1 : b.y >= a.y - 1) continue;
            const float dx = std::abs(b.cx() - a.cx());
            const bool closerRow = dir > 0 ? b.y < bestRow - 1 : b.y > bestRow + 1;
            const bool sameRow = std::abs(b.y - bestRow) <= 1;
            if (closerRow || (sameRow && dx < bestDx)) {
                best = static_cast<int>(i);
                bestRow = b.y;
                bestDx = dx;
            }
        }
        return best;
    }

    void activate(int i) {
        if (i < 0 || i >= static_cast<int>(tiles_.size())) return;
        const Tile t = tiles_[static_cast<size_t>(i)];
        if (t.add) {
            const std::string source = themes::chosen(t.light);
            Dispatcher::post([source] { themes::openEditor(source, true); });
            return;
        }
        themes::select(t.id);
        invalidate();
    }

    void menu(int i, gfx::Point at) {
        if (i < 0 || i >= static_cast<int>(tiles_.size())) return;
        const Tile t = tiles_[static_cast<size_t>(i)];
        const std::string id = t.add ? themes::chosen(t.light) : t.id;
        std::vector<ui::MenuItem> items;
        if (!t.add) items.push_back({tr(L"Kullan"), "check", L"", [id] { themes::select(id); }});
        if (ThemeStore::isCustom(id))
            items.push_back({tr(L"Düzenle"), "edit", L"", [id] { Dispatcher::post([id] { themes::openEditor(id, false); }); }});
        items.push_back({tr(L"Kopyala ve düzenle"), "plus", L"", [id] { Dispatcher::post([id] { themes::openEditor(id, true); }); }});
        items.push_back({tr(L"Dışa aktar…"), "share", L"", [id] { themes::exportTheme(id); }});
        if (ThemeStore::isCustom(id)) {
            items.push_back(ui::MenuItem::sep());
            ui::MenuItem del{tr(L"Sil"), "trash", L"", [id] { themes::confirmDelete(id); }};
            del.destructive = true;
            items.push_back(std::move(del));
        }
        ui::Menu::open(window(), at, std::move(items));
    }

    std::vector<Tile> tiles_;
    float darkLabelY_ = 0, lightLabelY_ = 0;
    int hot_ = -1, cursor_ = 0;
};

// ---- Segmented controls that follow changes made elsewhere ----------------------------------------------------------

class ModeChoice : public Segmented {
public:
    using Segmented::Segmented;
    void paint(Canvas& c) override {
        select(static_cast<int>(themeMode()));   // the palette's "Koyu tema" / Ctrl shortcut may change it
        Segmented::paint(c);
    }
};

// ---- Font picker -------------------------------------------------------------------------------------------------------

// The rows of the font list: the bundled font, Segoe UI Variable, then every installed family, each name in its own
// font. Only the visible rows are shaped (cached).
class FontRows : public ui::Widget {
public:
    FontRows() {
        focusable = true;
        all_.push_back({L"", tr(L"ShadeTube'un yazı tipi")});
        all_.push_back({gfx::Fonts::segoeFamily(), tr(L"Windows'un yazı tipi")});
        for (const auto& f : gfx::Fonts::installedFamilies())
            if (f != all_[1].family) all_.push_back({f, {}});
        setFilter({});
    }
    std::function<void(const std::wstring&)> onPick;
    static constexpr float kRowH = 40;

    void setFilter(const std::wstring& q) {
        const std::wstring fq = foldForSearch(q);
        shown_.clear();
        for (size_t i = 0; i < all_.size(); ++i) {
            const std::wstring name = i == 0 ? std::wstring(L"Bricolage Grotesque") : all_[i].family;
            if (fq.empty() || foldForSearch(name + L" " + all_[i].note).find(fq) != std::wstring::npos) shown_.push_back(i);
        }
        cursor_ = 0;
        for (size_t k = 0; k < shown_.size(); ++k)
            if (all_[shown_[k]].family == gfx::Fonts::uiFamily()) cursor_ = static_cast<int>(k);
        requestLayout();
        invalidate();
    }
    bool empty() const { return shown_.empty(); }
    void pickFirst() {
        if (!shown_.empty()) pick(cursor_);
    }
    float preferredHeight(float) override { return std::max(kRowH, static_cast<float>(shown_.size()) * kRowH); }
    bool activatable() const override { return true; }
    LPCWSTR cursor() const override { return IDC_HAND; }
    Rect focusRect() const override { return row(cursor_); }
    bool onKeyDown(const ui::KeyEvent& e) override {
        const int n = static_cast<int>(shown_.size());
        if (n == 0 || e.ctrl || e.alt) return false;
        int next = cursor_;
        switch (e.vk) {
        case VK_UP: next = std::max(0, cursor_ - 1); break;
        case VK_DOWN: next = std::min(n - 1, cursor_ + 1); break;
        case VK_PRIOR: next = std::max(0, cursor_ - 7); break;
        case VK_NEXT: next = std::min(n - 1, cursor_ + 7); break;
        case VK_HOME: next = 0; break;
        case VK_END: next = n - 1; break;
        default: return false;
        }
        cursor_ = next;
        scrollIntoView(row(cursor_));
        invalidate();
        return true;
    }
    bool onActivate() override {
        pick(cursor_);
        return true;
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        const int i = static_cast<int>((e.pos.y - rect().y) / kRowH);
        if (i < 0 || i >= static_cast<int>(shown_.size())) return false;
        cursor_ = i;
        pick(i);
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const int i = static_cast<int>((e.pos.y - rect().y) / kRowH);
        if (i != hot_) {
            hot_ = i;
            invalidate();
        }
    }
    void onMouseLeave() override {
        hot_ = -1;
        invalidate();
    }
    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect clip = c.clip();
        const int first = std::max(0, static_cast<int>((clip.y - rect().y) / kRowH));
        const int last = std::min(static_cast<int>(shown_.size()) - 1, static_cast<int>((clip.bottom() - rect().y) / kRowH));
        const std::wstring& current = gfx::Fonts::uiFamily();
        for (int k = first; k <= last; ++k) {
            const Entry& e = all_[shown_[static_cast<size_t>(k)]];
            const Rect r = row(k);
            const bool selected = e.family == current;
            if (k == hot_) c.fillRounded(r, gfx::radii().surface, col.overlayHover);
            if (selected) c.fillRounded(r, gfx::radii().surface, accent().tint06);
            auto& layout = layouts_[shown_[static_cast<size_t>(k)]];
            if (!layout) {
                const std::wstring family = e.family.empty() ? std::wstring(L"Bricolage Grotesque") : e.family;
                layout = gfx::makeFamilyLayout(family, family, 15, 500, r.w * 0.62f);
            }
            if (layout) {
                DWRITE_TEXT_METRICS m{};
                layout->GetMetrics(&m);
                c.text(layout.Get(), r.x + 12, r.cy() - m.height * 0.5f, selected ? accent().base : col.fgPrimary);
            }
            if (!e.note.empty())
                c.text(e.note, type::caption, {r.x + r.w * 0.62f + 16, r.y, r.w * 0.38f - 52, r.h}, col.fgTertiary,
                       gfx::TextAlign::Leading, gfx::VAlign::Center);
            if (selected) c.icon("check", {r.right() - 30, r.cy() - 9, 18, 18}, accent().base);
        }
    }

private:
    struct Entry {
        std::wstring family;   // "" = bundled
        std::wstring note;
    };
    Rect row(int k) const { return {rect().x, rect().y + static_cast<float>(k) * kRowH, rect().w, kRowH}; }
    void pick(int k) {
        if (k < 0 || k >= static_cast<int>(shown_.size())) return;
        const std::wstring family = all_[shown_[static_cast<size_t>(k)]].family;
        if (onPick) onPick(family);
        invalidate();
    }
    std::vector<Entry> all_;
    std::vector<size_t> shown_;
    std::unordered_map<size_t, Microsoft::WRL::ComPtr<IDWriteTextLayout4>> layouts_;
    int cursor_ = 0, hot_ = -1;
};

// ---- Theme editor ------------------------------------------------------------------------------------------------------

// Name, the five key colors (one selected, edited with the picker on the right) and the text contrast. Every change is
// previewed live on the whole app.
class ThemeEditor : public ui::Widget {
public:
    explicit ThemeEditor(ThemeSpec spec) : spec_(std::move(spec)) {
        hitTestVisible = false;
        nameLabel_ = add<ui::Label>(tr(L"Ad"), type::caption, ui::Tone::Secondary);
        name_ = add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Tema adı"));
        name_->setText(toWide(spec_.name));
        name_->onChange = [this](const std::wstring& v) {
            std::wstring n = v;
            while (!n.empty() && n.back() == L' ') n.pop_back();
            spec_.name = toUtf8(n.substr(0, 60));
        };
        for (const ThemeKey k : gfx::kThemeKeys) keys_.push_back(add<KeyRow>(this, k));
        picker_ = add<ColorPicker>(spec_.keys()[ThemeKey::Background]);
        picker_->onChange = [this](Color c) {
            gfx::setKey(spec_, selected_, c);
            themes::preview(spec_);
            invalidate();
        };
    }
    const ThemeSpec& spec() const { return spec_; }
    float preferredHeight(float) override { return 18 + 4 + 44 + 14 + 5 * kKeyH + 12 + 36; }
    void layout() override {
        const Rect r = rect();
        const float lw = 250;
        nameLabel_->setRect({r.x, r.y, lw, 18});
        name_->setRect({r.x, r.y + 22, lw, 44});
        float y = r.y + 22 + 44 + 14;
        for (auto* k : keys_) {
            k->setRect({r.x, y, lw, kKeyH});
            y += kKeyH;
        }
        const float px = r.x + lw + 28;
        picker_->setRect({px, r.y, r.right() - px, picker_->preferredHeight(r.right() - px)});
        contrastY_ = y + 12;
    }
    void paint(Canvas& c) override {
        paintChildren(c);
        const auto& col = colors();
        std::string why;
        const bool ok = gfx::readable(spec_, &why);
        const float ratio = gfx::contrastRatio(spec_.palette.fgPrimary, spec_.palette.bgBase);
        const Rect line{rect().x, contrastY_, 250, 36};
        c.icon(ok ? "check" : "warning", {line.x, line.y + 1, 16, 16}, ok ? col.success : col.error);
        c.text(i18n::format(tr(L"Metin karşıtlığı {}:1"), {ratioText(ratio)}), type::caption.withWeight(600),
               {line.x + 24, line.y, line.w - 24, 18}, col.fgPrimary);
        c.text(ok ? tr(L"Okunaklı: kaydedilebilir.") : tr(L"Kaydetmek için metin daha belirgin olmalı (en az 4,5:1)."),
               type::caption, {line.x + 24, line.y + 18, line.w - 24, 18}, col.fgSecondary);
    }
    void select(ThemeKey k) {
        selected_ = k;
        picker_->setColor(spec_.keys()[k]);
        invalidate();
    }

private:
    static constexpr float kKeyH = 36;
    class KeyRow : public ui::Widget {
    public:
        KeyRow(ThemeEditor* e, ThemeKey k) : e_(e), k_(k) { focusable = true; }
        bool activatable() const override { return true; }
        bool onActivate() override {
            e_->select(k_);
            return true;
        }
        bool onMouseDown(const ui::MouseEvent& e) override {
            if (e.button != ui::MouseButton::Left) return false;
            e_->select(k_);
            return true;
        }
        void onMouseEnter() override { invalidate(); }
        void onMouseLeave() override { invalidate(); }
        LPCWSTR cursor() const override { return IDC_HAND; }
        void paint(Canvas& c) override {
            const auto& col = colors();
            const Rect r = rect();
            const bool sel = e_->selected_ == k_;
            if (sel) c.fillRounded(r, gfx::radii().surface, col.overlaySelected);
            else if (hovered()) c.fillRounded(r, gfx::radii().surface, col.overlayHover);
            if (sel) c.fillRect({r.x, r.y + 8, 2, r.h - 16}, accent().base);
            const Color v = e_->spec_.keys()[k_];
            c.fillCircle({r.x + 22, r.cy()}, 9, v);
            c.strokeCircle({r.x + 22, r.cy()}, 9.5f, col.hairStrong);
            c.text(label(), type::secondary.withWeight(sel ? 600.f : 400.f), {r.x + 40, r.y, r.w - 120, r.h},
                   sel ? col.fgPrimary : col.fgSecondary, gfx::TextAlign::Leading, gfx::VAlign::Center);
            c.text(hexText(v), type::monoMeta, {r.right() - 80, r.y, 72, r.h}, col.fgTertiary, gfx::TextAlign::Trailing,
                   gfx::VAlign::Center);
        }

    private:
        std::wstring label() const {
            switch (k_) {
            case ThemeKey::Background: return tr(L"Arka plan");
            case ThemeKey::Surface: return tr(L"Yüzey");
            case ThemeKey::Text: return tr(L"Metin");
            case ThemeKey::Outline: return tr(L"Çizgiler");
            case ThemeKey::Accent: return tr(L"Vurgu");
            }
            return {};
        }
        ThemeEditor* e_;
        ThemeKey k_;
    };

    ThemeSpec spec_;
    ThemeKey selected_ = ThemeKey::Background;
    ui::Label* nameLabel_ = nullptr;
    ui::TextBox* name_ = nullptr;
    std::vector<KeyRow*> keys_;
    ColorPicker* picker_ = nullptr;
    float contrastY_ = 0;
};

std::wstring g_fontAtOpen;   // the font picker's "Vazgeç" goes back to it

} // namespace

// ---- Dialogs ---------------------------------------------------------------------------------------------------------

namespace themes {

void openAccentPicker() {
    auto* w = ctx().window;
    if (!w) return;
    auto* d = ui::Dialog::open(w, tr(L"Özel vurgu rengi"),
                               tr(L"Rengi karıştır ya da bir öneri seç; uygulama hemen önizlenir. Açık temalarda renk "
                                  L"okunaklı kalsın diye koyulaştırılır."),
                               460);
    if (!d) return;
    auto* body = static_cast<ui::Column*>(d->body());
    auto* picker = body->add<ColorPicker>(Settings::get().accentMode == AccentMode::Fixed ? fixedAccent()
                                                                                           : gfx::accent().base);
    picker->onChange = [](Color c) { previewAccent(c); };
    auto* row = body->add<SwatchRow>(suggestedColors(8), 30.f);
    row->onPick = [picker](Color c) {
        picker->setColor(c);
        previewAccent(c);
    };
    row->isSelected = [picker](Color c) { return sameRgb(c, picker->color()); };
    auto applied = std::make_shared<bool>(false);
    d->onClosed = [applied] {
        if (!*applied) restoreAccent();
    };
    d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
    d->addButton(tr(L"Uygula"), ButtonKind::Primary, [picker, applied] {
        *applied = true;
        setFixedAccent(picker->color());
        rowsChanged();
    });
}

void openFontPicker() {
    auto* w = ctx().window;
    if (!w) return;
    auto* d = ui::Dialog::open(w, tr(L"Arayüz yazı tipi"),
                               tr(L"Seçtiğin yazı tipi hemen uygulanır. Süreler ve etiketlerdeki eş aralıklı yazı değişmez."),
                               540);
    if (!d) return;
    g_fontAtOpen = gfx::Fonts::uiFamily();
    auto* body = static_cast<ui::Column*>(d->body());
    auto* search = body->add<ui::TextBox>(ui::TextBox::Look::Search, tr(L"Yazı tipi ara"));
    auto* box = body->add<ui::Box>();
    box->onPreferredHeight = [](float) { return 320.f; };
    auto* scroll = box->add<ui::ScrollView>();
    scroll->notchDip = 80;
    box->onLayout = [scroll](ui::Box& b) { scroll->setRect({0, 0, b.rect().w, b.rect().h}); };
    auto* rows = scroll->setContent<FontRows>();
    rows->onPick = [](const std::wstring& family) {
        if (!setUiFont(family)) toast(tr(L"Bu yazı tipi kullanılamıyor"), true);
    };
    search->onChange = [rows, scroll](const std::wstring& q) {
        rows->setFilter(q);
        scroll->contentChanged();
        scroll->scrollTo(0, false);
    };
    search->onSubmit = [rows](const std::wstring&) { rows->pickFirst(); };
    auto kept = std::make_shared<bool>(false);
    d->onClosed = [kept] {
        if (!*kept && gfx::Fonts::uiFamily() != g_fontAtOpen) setUiFont(g_fontAtOpen);
        rowsChanged();   // the row shows the font's name
    };
    d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
    d->addButton(tr(L"Tamam"), ButtonKind::Primary, [kept] { *kept = true; });
    // Scroll the chosen font into view once laid out.
    Dispatcher::post([rows, ref = std::weak_ptr<bool>(kept)] {
        if (!ref.expired()) rows->scrollIntoView(rows->focusRect());
    });
}

void openEditor(const std::string& id, bool duplicate) {
    auto* w = ctx().window;
    const gfx::ThemeSpec* src = store().find(id);
    if (!w || !src) return;
    ThemeSpec spec = duplicate ? store().duplicate(*src, toUtf8(i18n::format(tr(L"{} (kopya)"), {displayName(*src)})))
                               : *src;
    const bool isNew = duplicate || !ThemeStore::isCustom(id);
    auto* d = ui::Dialog::open(w, isNew ? tr(L"Yeni tema") : tr(L"Temayı düzenle"),
                               tr(L"Beş ana rengi değiştir; uygulama her değişiklikte önizlenir. Diğer tonlar bunlardan "
                                  L"türetilir."),
                               700);
    if (!d) return;
    auto* body = static_cast<ui::Column*>(d->body());
    auto* editor = body->add<ThemeEditor>(spec);
    preview(spec);
    auto saved = std::make_shared<bool>(false);
    d->onClosed = [saved] {
        if (!*saved) endPreview();
    };
    if (!isNew) {
        const std::string existing = id;
        d->addButton(tr(L"Sil"), ButtonKind::Ghost, [existing] {
            endPreview();
            Dispatcher::post([existing] { confirmDelete(existing); });
        });
    }
    d->addButton(tr(L"Vazgeç"), ButtonKind::Ghost, {});
    ui::Dialog* dialog = d;
    d->addButton(
        tr(L"Kaydet"), ButtonKind::Primary,
        [editor, saved, dialog] {
            ThemeSpec s = editor->spec();
            if (!gfx::readable(s)) {
                toast(tr(L"Metin arka planda okunamıyor; kaydetmeden önce renkleri değiştir."), true);
                return;
            }
            if (s.name.empty()) s.name = toUtf8(tr(L"Adsız tema"));
            *saved = true;
            endPreview();
            dialog->close();
            saveCustom(std::move(s));
        },
        false);
}

} // namespace themes

// ---- Ayarlar › Görünüm ---------------------------------------------------------------------------------------------------

namespace {

// Dev (screenshots): SHADETUBE_THEME_DIALOG = accent | font | editor:<id> | new:<id> opens that dialog when the rows are
// first built; SHADETUBE_THEME_IMPORT = <file> imports it then.
void runDevHooks() {
    static bool done = false;
    if (done) return;
    done = true;
    wchar_t buf[512];
    DWORD n = GetEnvironmentVariableW(L"SHADETUBE_THEME_IMPORT", buf, 512);
    if (n > 0 && n < 512) {
        const std::wstring file(buf, n);
        Dispatcher::post([file] { themes::importFile(file); });
    }
    n = GetEnvironmentVariableW(L"SHADETUBE_THEME_DIALOG", buf, 512);
    if (n == 0 || n >= 512) return;
    const std::string v = toUtf8(std::wstring_view(buf, n));
    Dispatcher::post([v] {
        if (v == "accent") themes::openAccentPicker();
        else if (v == "font") themes::openFontPicker();
        else if (v.starts_with("editor:")) themes::openEditor(v.substr(7), false);
        else if (v.starts_with("new:")) themes::openEditor(v.substr(4), true);
    });
}

} // namespace

void buildThemeStudioRows(ui::Column* c, const std::function<void()>& rebuild) {
    themes::setRowsChanged(rebuild);
    const auto& s = Settings::get();

    auto* modeRow = c->add<SettingRow>(
        tr(L"Tema"), tr(L"Koyu, açık ya da Windows'un uygulama moduna uyan sistem teması. Aşağıdan her mod için bir tema "
                        L"seç; değişiklik anında uygulanır."));
    // Theme names: dark / light / system ("Açık" here is the light theme, not "open").
    auto* mode = modeRow->control<ModeChoice>(220.f, std::vector<std::wstring>{tr(L"Koyu"), tr(L"Açık"), tr(L"Sistem")},
                                              static_cast<int>(themeMode()));
    mode->onChange = [](int i) {
        const ThemeMode m = i == 1 ? ThemeMode::Light : i == 2 ? ThemeMode::System : ThemeMode::Dark;
        if (m != themeMode()) setThemeMode(m);
    };
    c->add<ThemeGallery>();

    auto* fileRow = c->add<SettingRow>(
        tr(L"Tema dosyası"), tr(L"Kullandığın temayı paylaşmak için bir .json dosyası olarak dışa aktar ya da bir tema "
                                L"dosyasını içe aktar. Kendi temanı yapmak için bir temaya sağ tıkla."));
    auto* files = fileRow->control<ui::Box>(260.f);
    auto* imp = files->add<Button>(ButtonKind::Secondary, tr(L"İçe aktar…"), "download");
    auto* exp = files->add<Button>(ButtonKind::Secondary, tr(L"Dışa aktar…"), "share");
    files->onLayout = [imp, exp](ui::Box& b) {
        const float ew = std::ceil(exp->naturalWidth()), iw = std::ceil(imp->naturalWidth());
        const Rect r = b.rect();
        exp->setRect({r.w - ew, 0, ew, r.h});
        imp->setRect({r.w - ew - 8 - iw, 0, iw, r.h});
    };
    imp->onClick = [] { themes::importTheme(); };
    exp->onClick = [] { themes::exportTheme(gfx::Theme::get().themeId() == "preview" ? themes::chosen(gfx::isLightTheme())
                                                                                     : gfx::Theme::get().themeId()); };

    auto* srcRow = c->add<SettingRow>(
        tr(L"Vurgu rengi"), tr(L"Kapaktan: çalan şarkının kapağından alınır ve 600 ms'de yumuşakça geçer. Temadan: temanın "
                               L"kendi rengi. Sabit: aşağıda seçtiğin renk."));
    const int srcIndex = s.accentMode == AccentMode::Dynamic ? 0 : s.accentMode == AccentMode::Theme ? 1 : 2;
    auto* src = srcRow->control<Segmented>(260.f, std::vector<std::wstring>{tr(L"Kapaktan"), tr(L"Temadan"), tr(L"Sabit")},
                                           srcIndex);
    src->onChange = [](int i) {
        themes::setAccentMode(i == 0 ? AccentMode::Dynamic : i == 1 ? AccentMode::Theme : AccentMode::Fixed);
    };
    auto* fixedRow = c->add<SettingRow>(
        tr(L"Sabit vurgu rengi"), tr(L"Önerilerden birini seç ya da halkaya basıp kendi rengini karıştır. Seçince vurgu "
                                     L"rengi Sabit olur; açık temalarda okunaklı kalsın diye koyulaştırılır."));
    fixedRow->control<AccentSwatches>(240.f);

    auto* cornerRow = c->add<SettingRow>(
        tr(L"Köşeler"), tr(L"Kartların, kapakların, menülerin ve pencerelerin köşeleri. Düğmeler ve anahtarlar hap biçiminde "
                           L"kalır."));
    const gfx::CornerStyle cs = themes::cornerStyle();
    auto* corners = cornerRow->control<Segmented>(
        300.f, std::vector<std::wstring>{tr(L"Keskin"), tr(L"Varsayılan"), tr(L"Yuvarlak")},
        cs == gfx::CornerStyle::Sharp ? 0 : cs == gfx::CornerStyle::Round ? 2 : 1);
    corners->onChange = [](int i) {
        themes::setCornerStyle(i == 0 ? gfx::CornerStyle::Sharp : i == 2 ? gfx::CornerStyle::Round : gfx::CornerStyle::Default);
    };

    auto* fontRow = c->add<SettingRow>(
        tr(L"Yazı tipi"), tr(L"Arayüzün yazı tipi: ShadeTube'un kendi yazı tipi, Segoe UI Variable ya da bilgisayarında "
                             L"kurulu başka bir yazı tipi."));
    auto* font = fontRow->control<Button>(220.f, ButtonKind::Secondary, themes::uiFontLabel(), "chevron-down");
    font->onClick = [] { Dispatcher::post([] { themes::openFontPicker(); }); };

    runDevHooks();
}

} // namespace st::app
