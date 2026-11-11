// "Arayüz ölçeği" (UI zoom) and "Liste yoğunluğu" (track list density): settings rows, the zoom badge, Ctrl + wheel.
// See Display.h.
#include "app/Display.h"

#include "app/Commands.h"
#include "app/SettingsWidgets.h"
#include "app/Shortcuts.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "ui/Anim.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

namespace st::app {

namespace {

using gfx::colors;
namespace type = gfx::type;

std::wstring percentLabel(int percent) { return i18n::format(tr(L"%{}"), percent); }   // "%110" / "110%"

// The zoom feedback: a small surface under the title bar ("ARAYÜZ ÖLÇEĞİ  %110"), fading in, held for a moment and
// fading out (no fades with "Hareketi azalt": the motion durations collapse to 0). One per window, updated in place
// by every step, so holding the keys never stacks toasts. Never takes the pointer or keys.
class ZoomBadge : public ui::Widget {
public:
    ZoomBadge() : label_(toUpperTr(tr(L"Arayüz ölçeği")), type::monoLabel), value_({}, type::bodyL.withWeight(700)) {
        hitTestVisible = false;
    }
    void show(int percent) {
        value_.setText(percentLabel(percent));
        shownAt_ = ui::frame::realNow();
        alpha_.to(1, ui::motion::fast, ui::Ease::Decelerate);
        if (auto* w = window()) w->invalidateAfter(kHoldMs + 1);   // the fade-out starts without input
        invalidate();
    }
    Widget* hitTest(gfx::Point) override { return nullptr; }
    void paint(Canvas& c) override {
        if (alpha_.target() > 0 && ui::frame::realNow() - shownAt_ > kHoldMs) alpha_.to(0, ui::motion::normal);
        const float a = alpha_;
        if (a <= 0.001f) return;
        const auto& col = colors();
        const float lw = std::ceil(label_.measure().w), vw = std::ceil(value_.measure().w);
        const float w = 16 + lw + 12 + vw + 16, h = 40;
        const Rect r{std::round(rect().cx() - w * 0.5f), gfx::metrics::titleBarH + 12, w, h};
        c.pushOpacity(a);
        c.shadow(r, 2, 24, 8, col.shadowToast);
        c.fillRounded(r, 2, col.bgElevated);
        c.strokeRounded(r, 2, col.hairStrong);
        c.text(label_, {r.x + 16, r.y, lw + 1, h}, col.fgTertiary, gfx::VAlign::Center);
        c.text(value_, {r.x + 16 + lw + 12, r.y, vw + 1, h}, col.fgPrimary, gfx::VAlign::Center);
        c.popLayer();
    }

private:
    static constexpr double kHoldMs = 1100;
    gfx::Text label_, value_;
    ui::Anim alpha_;
    double shownAt_ = 0;
};

ZoomBadge* g_badge = nullptr;   // in ctx().window's overlay layer (lives as long as the window)
ui::Window* g_badgeWindow = nullptr;

void showBadge(ui::Window* w, int percent) {
    if (!g_badge || g_badgeWindow != w) {
        g_badge = static_cast<ZoomBadge*>(w->pushOverlay(std::make_unique<ZoomBadge>(), false));
        g_badgeWindow = w;
    }
    g_badge->show(percent);
}

// The rows' controls follow a change made elsewhere (shortcut, wheel, palette) while the page is open.
class ZoomChoice : public Segmented {
public:
    ZoomChoice(std::vector<std::wstring> items, int selected) : Segmented(std::move(items), selected) {
        onChange = [this](int i) {
            if (display::kZoomSteps[i] == display::zoom()) return;   // following a change made elsewhere
            display::setZoom(display::kZoomSteps[i], false);
            reveal_ = true;
        };
    }
    void paint(Canvas& c) override {
        select(display::zoomIndex(display::zoom()));   // no-op when it already shows it
        // Picked here: the page has reflowed for the new zoom (layout runs before paint), keep this row in sight.
        if (reveal_) {
            reveal_ = false;
            if (Widget* row = parent()) row->scrollIntoView(row->rect());
        }
        Segmented::paint(c);
    }

private:
    bool reveal_ = false;
};

class DensityChoice : public Segmented {
public:
    using Segmented::Segmented;
    void paint(Canvas& c) override {
        select(display::compactLists() ? 1 : 0);
        Segmented::paint(c);
    }
};

} // namespace

namespace display {

int zoom() { return snapZoom(Settings::get().uiZoom); }

void setZoom(int percent, bool feedback) {
    percent = snapZoom(percent);
    auto& s = Settings::get();
    if (s.uiZoom != percent) {
        s.uiZoom = percent;
        s.markDirty();
    }
    auto* w = ctx().window;
    if (!w) return;
    w->setZoom(static_cast<float>(percent) / 100.f);
    if (feedback) showBadge(w, percent);
}

bool zoomBy(int dir) {
    setZoom(stepZoom(zoom(), dir), true);   // at an end the badge still says where it is
    return true;
}

bool onCtrlWheel(float delta) {
    static float pending = 0;
    static double last = 0;
    const double now = ui::frame::realNow();
    if (now - last > 500 || (pending > 0) != (delta > 0)) pending = 0;   // a new gesture, or the other way
    last = now;
    pending += delta;
    if (std::abs(pending) >= 1.f) {
        zoomBy(pending > 0 ? 1 : -1);
        pending = 0;
    }
    return true;
}

namespace {
std::vector<std::string> g_demoSteps;
size_t g_demoNext = 0;

void CALLBACK demoTick(HWND, UINT, UINT_PTR id, DWORD) {
    if (g_demoNext >= g_demoSteps.size()) {
        KillTimer(nullptr, id);
        return;
    }
    SetTimer(nullptr, id, 300, demoTick);   // the same timer, from now on every 300 ms
    const std::string step = g_demoSteps[g_demoNext++];
    if (step.rfind("action:", 0) == 0) {
        ST_LOG_INFO("display", "keys demo: run {}", step.substr(7));
        commands::run(step.substr(7));
        return;
    }
    const auto combo = shortcuts::parse(step);
    if (!combo || combo->empty()) {
        ST_LOG_WARN("display", "keys demo: \"{}\" is not a key combo", step);
        return;
    }
    ui::KeyEvent e;
    e.vk = combo->vk;
    e.ctrl = combo->ctrl;
    e.alt = combo->alt;
    e.shift = combo->shift;
    const bool handled = commands::dispatchKey(e, commands::Scope::Main, false);
    ST_LOG_INFO("display", "keys demo: {} {}", step, handled ? "handled" : "not handled");
    if (auto* w = ctx().window) w->invalidate();
}
} // namespace

void scheduleKeysDemo() {
    wchar_t spec[512] = {};
    if (!GetEnvironmentVariableW(L"SHADETUBE_KEYS_DEMO", spec, 512)) return;
    const std::string s = toUtf8(spec);
    const size_t colon = s.find(':');
    const int delay = colon == std::string::npos ? 1500 : std::max(0, std::atoi(s.substr(0, colon).c_str()));
    for (size_t start = colon == std::string::npos ? 0 : colon + 1; start < s.size();) {
        const size_t comma = std::min(s.find(',', start), s.size());
        if (comma > start) g_demoSteps.push_back(s.substr(start, comma - start));
        start = comma + 1;
    }
    SetTimer(nullptr, 0, static_cast<UINT>(delay), demoTick);
}

bool compactLists() { return Settings::get().compactLists; }

void setCompactLists(bool on) {
    auto& s = Settings::get();
    if (s.compactLists == on) return;
    s.compactLists = on;
    s.markDirty();
    // Every track table's rows change height: the pages around them lay out again.
    if (auto* w = ctx().window) w->relayoutAll();
}

} // namespace display

// Ayarlar › Görünüm.
void buildDisplayRows(ui::Column* c, const std::function<void()>&) {
    // The keys come from the user's bindings (Ayarlar › KLAVYE): "Ctrl+= / Ctrl+-, Ctrl+0".
    const auto& o = Settings::get().shortcuts;
    const std::wstring in = shortcuts::display(shortcuts::binding(o, "zoom-in", false)),
                       out = shortcuts::display(shortcuts::binding(o, "zoom-out", false)),
                       reset = shortcuts::display(shortcuts::binding(o, "zoom-reset", false));
    std::wstring desc = tr(L"Yazılar, simgeler ve boşluklar Windows ölçeğinden bağımsız olarak büyür ya da küçülür; "
                           L"pencerenin boyutu değişmez.");
    if (!in.empty() && !out.empty() && !reset.empty())
        desc += L" " + i18n::format(tr(L"Klavyeden {} ve {}, varsayılana dönmek için {}; Ctrl + fare tekerleği de çalışır."),
                                    {in, out, reset});
    else
        desc += L" " + std::wstring(tr(L"Ctrl + fare tekerleği de çalışır."));
    auto* zoomRow = c->add<SettingRow>(tr(L"Arayüz ölçeği"), desc);
    std::vector<std::wstring> steps;
    for (int p : display::kZoomSteps) steps.push_back(percentLabel(p));
    zoomRow->control<ZoomChoice>(300.f, std::move(steps), display::zoomIndex(display::zoom()));

    auto* densityRow = c->add<SettingRow>(
        tr(L"Liste yoğunluğu"), tr(L"Şarkı listelerinin satır yüksekliği. Sıkı görünümde satırlar alçalır ve kapaklar "
                                   L"küçülür; ekrana daha çok şarkı sığar."));
    auto* density = densityRow->control<DensityChoice>(
        160.f, std::vector<std::wstring>{tr(L"Rahat"), tr(L"Sıkı")}, display::compactLists() ? 1 : 0);
    density->onChange = [](int i) { display::setCompactLists(i == 1); };
}

} // namespace st::app
