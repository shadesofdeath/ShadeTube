// Ayarlar > PENCERE (start with Windows / in the tray) and KLAVYE (keyboard shortcuts, global hotkeys).
#include "app/Autostart.h"
#include "app/Commands.h"
#include "app/SettingsWidgets.h"
#include "app/Shortcuts.h"
#include "core/I18n.h"
#include "ui/Popups.h"
#include "ui/Window.h"

namespace st::app {

namespace {

using gfx::accent;
using gfx::colors;
namespace type = gfx::type;
using shortcuts::Combo;

std::wstring problemText(shortcuts::Problem p) {
    switch (p) {
    case shortcuts::Problem::Reserved: return tr(L"Bu tuş klavyeyle gezinmeye ve Windows'a ayrılmış.");
    case shortcuts::Problem::NeedsModifier:
        return tr(L"Genel kısayollarda Ctrl, Alt ya da Win tuşlarından biri gerekir (F tuşları hariç).");
    case shortcuts::Problem::WinKey: return tr(L"Win tuşlu birleşimler Windows'a ayrılmış; uygulama içinde kullanılamaz.");
    case shortcuts::Problem::MediaKey: return tr(L"Medya tuşları zaten Windows'un medya denetimleriyle çalışıyor.");
    case shortcuts::Problem::None: break;
    }
    return {};
}

// The combo of one action as a pill; click (or Enter / Space) records a new one: the next key chord is taken,
// Esc cancels, Backspace / Delete clears. While recording it takes every key (not activatable), so Space, arrows
// and Enter can be bound too; losing focus (click elsewhere, Tab) cancels.
class ComboButton : public ui::Widget {
public:
    explicit ComboButton(bool global) : global_(global) {
        focusable = true;
        text_.setOptions({gfx::TextAlign::Center});
    }
    std::function<void(const Combo&)> onRecorded;

    void setCombo(const Combo& c) {
        text_.setText(c.empty() ? std::wstring(tr(L"Atanmamış")) : shortcuts::display(c));
        empty_ = c.empty();
        invalidate();
    }

    bool activatable() const override { return !recording_; }
    bool onActivate() override {
        start();
        return true;
    }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (!pill().contains(e.pos)) return;
        focus();
        start();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    void onFocusChanged(bool focused) override {
        if (!focused) stop();
    }
    Rect focusRect() const override { return pill(); }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Pill; }

    bool onKeyDown(const ui::KeyEvent& e) override {
        if (!recording_) return false;
        if (shortcuts::isModifierKey(e.vk) || e.repeat) return true;
        const bool win = GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0;
        const bool plain = !e.ctrl && !e.alt && !e.shift && !win;
        if (e.vk == VK_ESCAPE && plain) {
            stop();
            return true;
        }
        if ((e.vk == VK_BACK || e.vk == VK_DELETE) && plain) {
            stop();
            if (onRecorded) {
                const auto cb = onRecorded;   // may rebuild the rows
                cb(Combo{});
            }
            return true;
        }
        Combo c{e.vk, e.ctrl, e.alt, e.shift, win};
        if (shortcuts::format(c).empty()) return true;   // a key without a name (OEM / IME): keep waiting
        if (const auto p = shortcuts::check(c, global_); p != shortcuts::Problem::None) {
            toast(problemText(p), true);
            return true;
        }
        stop();
        if (onRecorded) {
            const auto cb = onRecorded;
            cb(c);
        }
        return true;
    }

    void paint(Canvas& c) override {
        const auto& col = colors();
        const Rect r = pill();
        const float h = hover_;
        if (recording_) {
            c.fillPill(r, accent().tint12);
            c.strokePill(r, accent().base, 1.5f);
            c.text(tr(L"Tuşlara bas…"), type::monoMeta, r.inset(14, 0), accent().base, gfx::TextAlign::Center, gfx::VAlign::Center);
            return;
        }
        c.fillPill(r, col.overlayHover.mulAlpha(h));
        c.strokePill(r, Color::lerp(col.hairControl, col.fgSecondary.withAlpha(0.6f), h));
        c.text(text_, r.inset(14, 0), empty_ ? col.fgTertiary : col.fgPrimary, gfx::VAlign::Center);
    }

private:
    Rect pill() const { return rect().center(rect().w, 32); }
    void start() {
        recording_ = true;
        invalidate();
    }
    void stop() {
        if (!recording_) return;
        recording_ = false;
        invalidate();
    }
    bool global_;
    bool recording_ = false, empty_ = false;
    gfx::Text text_{L"", type::monoMeta};
    ui::Anim hover_;
};

// One action: name on the left, [reset] [combo] on the right, hairline below.
class ShortcutRow : public ui::Widget {
public:
    ShortcutRow(const shortcuts::Action& a, bool global, std::function<void()> refreshAll)
        : action_(a), global_(global), refreshAll_(std::move(refreshAll)), name_(a.name(), type::bodyRegular),
          error_(tr(L"Başka bir uygulama bu birleşimi kullanıyor"), type::caption) {
        hitTestVisible = false;
        combo_ = add<ComboButton>(global);
        combo_->onRecorded = [this](const Combo& c) { apply(c); };
        reset_ = add<ui::Button>(ui::ButtonKind::Icon, L"", global ? "close" : "refresh");
        reset_->setTooltip(global ? tr(L"Kısayolu kaldır") : tr(L"Varsayılana döndür"));
        reset_->onClick = [this] { resetBinding(); };
        refresh();
    }
    void refresh() {
        const auto& o = Settings::get().shortcuts;
        combo_->setCombo(shortcuts::binding(o, action_.id, global_));
        reset_->setVisible(!shortcuts::isDefault(o, action_.id, global_));
        failed_ = global_ && commands::globalHotkeyFailed(action_.id);
        invalidate();
    }
    float preferredHeight(float) override { return failed_ ? 60.f : 48.f; }
    void layout() override {
        const Rect r = rect();
        const float cw = std::max(168.f, std::ceil(comboWidth()));
        combo_->setRect({r.w - cw, 0, cw, r.h});
        reset_->setRect({r.w - cw - 8 - 32, r.h * 0.5f - 16, 32, 32});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float tw = r.w - 168 - 56;
        if (failed_) {
            c.text(name_, {r.x, r.y + 10, tw, 20}, col.fgPrimary, gfx::VAlign::Center);
            c.text(error_, {r.x, r.y + 32, tw, 16}, col.error, gfx::VAlign::Center);
        } else {
            c.text(name_, {r.x, r.y, tw, r.h}, col.fgPrimary, gfx::VAlign::Center);
        }
        c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
        paintChildren(c);
    }

private:
    float comboWidth() {
        gfx::Text t(shortcuts::display(shortcuts::binding(Settings::get().shortcuts, action_.id, global_)), type::monoMeta);
        return t.measure().w + 40;
    }
    void apply(const Combo& c) {
        auto& s = Settings::get();
        const auto displaced = shortcuts::assign(s.shortcuts, action_.id, c, global_);
        s.markDirty();
        if (global_) commands::applyGlobalHotkeys();
        if (!displaced.empty())
            if (const auto* other = shortcuts::find(displaced.front()))
                toast(i18n::format(tr(L"{} artık bu işlem için. \"{}\" kısayolsuz kaldı."), {shortcuts::display(c), other->name()}));
        if (global_ && !c.empty() && commands::globalHotkeyFailed(action_.id))
            toast(tr(L"Bu birleşimi başka bir uygulama kullanıyor. Başka bir birleşim dene."), true);
        const auto refresh = refreshAll_;   // relayouts every row
        refresh();
    }
    void resetBinding() {
        auto& s = Settings::get();
        const auto displaced = shortcuts::reset(s.shortcuts, action_.id, global_);
        s.markDirty();
        if (global_) commands::applyGlobalHotkeys();
        if (!displaced.empty())
            if (const auto* other = shortcuts::find(displaced.front()))
                toast(i18n::format(tr(L"\"{}\" kısayolsuz kaldı."), {other->name()}));
        const auto refresh = refreshAll_;
        refresh();
    }

    const shortcuts::Action& action_;
    bool global_;
    std::function<void()> refreshAll_;
    ComboButton* combo_;
    ui::Button* reset_;
    gfx::Text name_, error_;
    bool failed_ = false;
};

// The rows of the KLAVYE section: refreshing one binding may change others (a taken combo moves).
class ShortcutList : public ui::Column {
public:
    ShortcutList() : ui::Column(0) {}
    void refresh() {
        for (const auto& ch : children())
            if (auto* row = dynamic_cast<ShortcutRow*>(ch.get())) row->refresh();
        requestLayout();
        invalidate();
    }
};

// A group of rows ("Oynatma") inside the section.
void groupLabel(ui::Column* c, std::wstring text) {
    c->add<Spacer>(20.f);
    c->add<ui::Label>(std::move(text), type::secondary.withWeight(600), ui::Tone::Secondary);
    c->add<Spacer>(4.f);
}

} // namespace

void buildStartupRows(ui::Column* c, const std::function<void()>& rebuild) {
    auto& s = Settings::get();
    const bool blocked = s.startWithWindows && autostart::disabledByWindows();
    std::wstring desc = tr(L"Windows'ta oturum açınca ShadeTube kendiliğinden açılır.");
    if (blocked)
        desc += L" " + std::wstring(tr(L"Şu an Windows'un Başlangıç uygulamaları ayarında kapalı; kapatıp açarsan yeniden "
                                      L"etkinleşir."));
    settingsToggle(c, tr(L"Windows ile başlat"), desc, s.startWithWindows, [rebuild](bool v) {
        Settings::get().startWithWindows = v;
        if (autostart::allowed()) {
            if (v) autostart::clearWindowsDisabled();
            const bool now = autostart::sync(v, commands::autostartExe());
            if (v && !now) toast(tr(L"Başlangıç kaydı yazılamadı."), true);
        }
        rebuild();
    });
    auto* tray = settingsToggle(c, tr(L"Tepside başlat"),
                                tr(L"Windows ile açılırken pencere gösterilmez: ShadeTube bildirim alanında bekler, "
                                   L"kaldığın sıra yüklü ama duraklatılmış olur."),
                                s.startInTray, [](bool v) { Settings::get().startInTray = v; });
    tray->setEnabled(s.startWithWindows);
}

void buildShortcutRows(ui::Column* c, const std::function<void()>&) {
    const auto& o = Settings::get().shortcuts;
    auto* intro = c->add<SettingRow>(
        tr(L"Klavye kısayolları"),
        i18n::format(tr(L"Değiştirmek için bir kısayola tıkla ve yeni tuş birleşimine bas: Esc vazgeçer, Backspace "
                        L"kısayolu kaldırır. Komut paleti ({}) her şeyi tek yerden açar."),
                     {shortcuts::display(shortcuts::binding(o, "command-palette", false))}));
    auto* resetAll = intro->control<ui::Button>(150.f, ui::ButtonKind::Secondary, tr(L"Tümünü sıfırla"), "refresh");
    auto* list = c->add<ShortcutList>();
    const auto refreshAll = [list] { list->refresh(); };
    resetAll->onClick = [list] {
        auto& s = Settings::get();
        shortcuts::resetAll(s.shortcuts);
        s.markDirty();
        commands::applyGlobalHotkeys();
        list->refresh();
        toast(tr(L"Kısayollar varsayılana döndü"));
    };

    for (auto cat : {shortcuts::Category::Playback, shortcuts::Category::Navigation, shortcuts::Category::View}) {
        groupLabel(list, shortcuts::categoryName(cat));
        for (const auto& a : shortcuts::actions())
            if (a.category == cat && !(a.flags & shortcuts::kGlobalOnly)) list->add<ShortcutRow>(a, false, refreshAll);
    }

    // Global hotkeys: no defaults, registered on the main window for the whole run.
    groupLabel(list, tr(L"Genel kısayollar"));
    list->add<SettingRow>(tr(L"Uygulama arka plandayken de çalışır"),
                          tr(L"ShadeTube başka bir pencerenin arkasında ya da tepsideyken de bu birleşimlere cevap verir. "
                             L"Ctrl, Alt ya da Win tuşlarından birini kullan; medya tuşları zaten her zaman çalışır."));
    for (const auto& a : shortcuts::actions())
        if (a.flags & shortcuts::kGlobal) list->add<ShortcutRow>(a, true, refreshAll);
}

} // namespace st::app
