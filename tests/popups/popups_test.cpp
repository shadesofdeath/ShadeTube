// Developer test for the overlay family (ui/Popups, ui/Acrylic, the Window's overlay layer) on a hidden window, offline.
// Frames are drawn with Window::render() (the real path: captures, exit animations) and read back with renderToPng()
// into a per-process temp folder.
//   menu        keyboard as before: Down / Up wrap over disabled items and separators, Home / End, Tab, Enter runs the
//               action and closes, Escape closes, a menu opened from the keyboard lights its first item; separators
//               are tidied (none leading, trailing or doubled)
//   submenu     Right / Enter open it with its first item lit, the parent item stays lit, Left / Escape close only
//               the submenu, an item inside runs its action; with the pointer: resting on the item opens it, resting
//               on another one closes it, a click runs an item
//   typeahead   letters jump to the next item starting with them (case / accent insensitive), the same letter
//               cycles, disabled items are skipped
//   exit        close() keeps the menu painted without input for its animation, then deletes it and frees its
//               backdrop; Hareketi azalt (no animation) removes it at once
//   acrylic     one capture when it opens, none for further frames, one more after something behind changed and
//               settled; the material shows what is behind (a red / blue backdrop) while staying near the surface
//               color (fg.secondary keeps >= 4.5:1); `opaque` (child process, SHADETUBE_ACRYLIC=off): no capture,
//               no bitmap, an even surface
//   toasts      a fourth toast sends the oldest out, the stack settles at three
//   dialog      Enter presses the default button, Escape closes it; both leave through the exit animation
//   popups_test [opaque]
#include "core/Resources.h"
#include "gfx/Device.h"
#include "gfx/Text.h"
#include "gfx/Theme.h"
#include "ui/Acrylic.h"
#include "ui/Anim.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace st;
using namespace st::ui;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (cond) {                                                                       \
            std::printf("  ok    %s\n", #cond);                                           \
        } else {                                                                          \
            std::printf("  FAIL  %s  (line %d)\n", #cond, __LINE__);                      \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

std::filesystem::path g_dir;

// The page behind: left half red, right half blue (what the acrylic should let through), and a small widget in the
// bottom-right corner that repaints on its own (a ticking label far from the menus).
class Backdrop : public Widget {
public:
    Backdrop() { ticker = add<Widget>(); }
    void layout() override { ticker->setRect({rect().right() - 60, rect().bottom() - 60, 40, 40}); }
    Widget* ticker = nullptr;
    void paint(Canvas& c) override {
        const Rect r = rect();
        c.fillRect({r.x, r.y, r.w * 0.5f, r.h}, Color::rgb(0xE03020));
        c.fillRect({r.cx(), r.y, r.w * 0.5f, r.h}, Color::rgb(0x2040E0));
    }
};

struct Image {
    UINT w = 0, h = 0;
    std::vector<uint8_t> bgra;
    Color at(float x, float y) const {
        const UINT px = std::min(w - 1, static_cast<UINT>(std::max(0.f, x))), py = std::min(h - 1, static_cast<UINT>(std::max(0.f, y)));
        const uint8_t* p = &bgra[(static_cast<size_t>(py) * w + px) * 4];
        return {p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, 1.f};
    }
};

Image shot(Window& w, const char* name) {
    Image img;
    const auto path = g_dir / (std::string(name) + ".png");
    if (!w.renderToPng(path.wstring())) return img;
    auto* wic = gfx::Device::get().wic();
    gfx::ComPtr<IWICBitmapDecoder> dec;
    gfx::ComPtr<IWICBitmapFrameDecode> frame;
    gfx::ComPtr<IWICFormatConverter> conv;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(wic->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)) ||
        FAILED(conv->GetSize(&img.w, &img.h)))
        return {};
    img.bgra.resize(static_cast<size_t>(img.w) * img.h * 4);
    if (FAILED(conv->CopyPixels(nullptr, img.w * 4, static_cast<UINT>(img.bgra.size()), img.bgra.data()))) return {};
    return img;
}

void key(Window& w, UINT vk) {
    w.handleMessageThunk(w.hwnd(), WM_KEYDOWN, vk, 1);
    w.handleMessageThunk(w.hwnd(), WM_KEYUP, vk, 0xC0000001);
}

void click(Window& w, Point dip) {
    const float s = w.scale();
    const LPARAM lp = MAKELPARAM(static_cast<int>(dip.x * s), static_cast<int>(dip.y * s));
    w.handleMessageThunk(w.hwnd(), WM_MOUSEMOVE, 0, lp);
    w.handleMessageThunk(w.hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, lp);
    w.handleMessageThunk(w.hwnd(), WM_LBUTTONUP, 0, lp);
}

void frames(Window& w, int n) {
    for (int i = 0; i < n; ++i) {
        w.invalidate();
        w.render();
    }
}

void waitFrames(Window& w, double ms) {
    const double until = frame::realNow() + ms;
    while (frame::realNow() < until) {
        Sleep(8);
        w.render();
    }
}

Point center(const Rect& r) { return {r.cx(), r.cy()}; }

float contrast(const Color& a, const Color& b) {
    const float la = a.luminance(), lb = b.luminance();
    return (std::max(la, lb) + 0.05f) / (std::min(la, lb) + 0.05f);
}

std::string g_ran;

std::vector<MenuItem> sampleItems() {
    std::vector<MenuItem> items;
    items.push_back({L"Sonra çal", "queue", L"", [] { g_ran = "sonra"; }});
    items.push_back(MenuItem::sep());
    items.push_back(MenuItem::sep());   // doubled: dropped
    MenuItem off{L"Sıraya ekle", "plus", L"", [] { g_ran = "sira"; }};
    off.enabled = false;
    items.push_back(off);
    items.push_back({L"Şarkı radyosu", "info", L"Ctrl+R", [] { g_ran = "radyo"; }});
    MenuItem sub{L"Çalma listesine ekle…", "playlist", L"", {}};
    sub.submenu = [] {
        return std::vector<MenuItem>{{L"Yeni çalma listesi…", "plus", L"", [] { g_ran = "yeni"; }},
                                     {L"Gece", "playlist", L"", [] { g_ran = "gece"; }},
                                     {L"Sabah", "playlist", L"", [] { g_ran = "sabah"; }}};
    };
    items.push_back(sub);
    items.push_back({L"İndir", "check", L"", [] { g_ran = "indir"; }});
    items.push_back(MenuItem::sep());   // trailing: dropped
    return items;
}

// ---------------------------------------------------------------------------------------------------

void testMenuKeys(Window& w) {
    std::printf("menu\n");
    Menu* m = Menu::open(&w, {300, 100}, sampleItems());
    frames(w, 1);
    CHECK(w.hasModal());
    CHECK(m->highlighted(0) == -1);   // a mouse-opened menu waits for the pointer
    // Items after tidying: 0 Sonra çal, 1 ---, 2 Sıraya ekle (disabled), 3 Şarkı radyosu, 4 Çalma listesine ekle >, 5 İndir.
    CHECK(m->itemRect(0, 5).h > 0 && m->itemRect(0, 6).h == 0);
    key(w, VK_DOWN);
    CHECK(m->highlighted(0) == 0);
    key(w, VK_DOWN);
    CHECK(m->highlighted(0) == 3);   // over the separator and the disabled item
    key(w, VK_DOWN);
    key(w, VK_DOWN);
    CHECK(m->highlighted(0) == 5);
    key(w, VK_DOWN);
    CHECK(m->highlighted(0) == 0);   // wraps
    key(w, VK_UP);
    CHECK(m->highlighted(0) == 5);
    key(w, VK_HOME);
    CHECK(m->highlighted(0) == 0);
    key(w, VK_END);
    CHECK(m->highlighted(0) == 5);
    key(w, VK_TAB);
    CHECK(m->highlighted(0) == 0);   // Tab walks the items like Down
    g_ran.clear();
    key(w, VK_HOME);
    key(w, VK_DOWN);   // Şarkı radyosu
    key(w, VK_RETURN);
    CHECK(g_ran == "radyo");
    CHECK(!w.hasModal());
    waitFrames(w, 200);

    // Escape closes without running anything.
    g_ran.clear();
    Menu::open(&w, {300, 100}, sampleItems());
    frames(w, 1);
    key(w, VK_ESCAPE);
    CHECK(!w.hasModal());
    CHECK(g_ran.empty());
    waitFrames(w, 200);
}

void testSubmenu(Window& w) {
    std::printf("submenu\n");
    g_ran.clear();
    Menu* m = Menu::open(&w, {300, 100}, sampleItems());
    frames(w, 1);
    key(w, VK_END);
    key(w, VK_UP);   // Çalma listesine ekle
    CHECK(m->highlighted(0) == 4);
    key(w, VK_RIGHT);
    frames(w, 1);
    CHECK(m->depth() == 2);
    CHECK(m->highlighted(1) == 0);
    CHECK(m->highlighted(0) == 4);   // the parent item stays lit
    CHECK(m->panelRect(1).x >= m->panelRect(0).right() || m->panelRect(1).right() <= m->panelRect(0).x);
    key(w, VK_LEFT);
    CHECK(m->depth() == 1);
    CHECK(m->highlighted(0) == 4);
    key(w, VK_RETURN);   // Enter on a submenu item opens it
    CHECK(m->depth() == 2);
    key(w, VK_ESCAPE);   // closes only the submenu
    CHECK(m->depth() == 1);
    CHECK(w.hasModal());
    key(w, VK_RIGHT);
    key(w, VK_DOWN);     // Gece
    key(w, VK_RETURN);
    CHECK(g_ran == "gece");
    CHECK(!w.hasModal());
    waitFrames(w, 200);

    // The pointer: resting on the item opens the submenu, resting on another item closes it, a click runs one.
    g_ran.clear();
    m = Menu::open(&w, {300, 100}, sampleItems());
    frames(w, 1);
    w.simulatePointer(center(m->itemRect(0, 4)));
    frames(w, 1);
    CHECK(m->depth() == 1);   // not at once
    waitFrames(w, 260);
    CHECK(m->depth() == 2);
    w.simulatePointer(center(m->itemRect(1, 2)));   // into the submenu: it stays
    waitFrames(w, 350);
    CHECK(m->depth() == 2);
    CHECK(m->highlighted(1) == 2);
    CHECK(m->highlighted(0) == 4);
    w.simulatePointer(center(m->itemRect(0, 0)));   // another item of the parent
    waitFrames(w, 380);
    CHECK(m->depth() == 1);
    CHECK(m->highlighted(0) == 0);
    click(w, center(m->itemRect(0, 5)));
    CHECK(g_ran == "indir");
    CHECK(!w.hasModal());
    waitFrames(w, 200);
}

void testTypeAhead(Window& w) {
    std::printf("typeahead\n");
    Menu* m = Menu::open(&w, {300, 100}, sampleItems());
    frames(w, 1);
    key(w, 'S');
    CHECK(m->highlighted(0) == 0);   // "Sonra çal"
    key(w, 'S');
    CHECK(m->highlighted(0) == 3);   // the same letter again: the next one, "Şarkı radyosu" (accent-insensitive; the
                                     // disabled "Sıraya ekle" skipped)
    key(w, 'S');
    CHECK(m->highlighted(0) == 0);   // round again
    Sleep(900);                      // a new word
    key(w, 'C');
    CHECK(m->highlighted(0) == 4);   // "Çalma listesine ekle"
    key(w, 'A');
    CHECK(m->highlighted(0) == 4);   // "ça": still the same item
    key(w, 'X');
    CHECK(m->highlighted(0) == 4);   // no match: stays
    CHECK(w.hasModal());             // letters never close the menu
    key(w, VK_ESCAPE);
    waitFrames(w, 200);
}

void testExitAndAcrylic(Window& w, bool opaque) {
    std::printf(opaque ? "opaque\n" : "acrylic + exit\n");
    frames(w, 2);
    const int c0 = Acrylic::captureCount();
    Menu* m = Menu::open(&w, {300, 100}, sampleItems());
    frames(w, 1);
    if (opaque) {
        CHECK(Acrylic::captureCount() == c0);
        CHECK(Acrylic::liveBytes() == 0);
    } else {
        CHECK(Acrylic::captureCount() == c0 + 1);
        CHECK(Acrylic::liveBytes() > 0);
    }
    frames(w, 10);
    CHECK(Acrylic::captureCount() == c0 + (opaque ? 0 : 1));   // not per frame
    waitFrames(w, 250);                                         // the open animation ends
    const Image img = shot(w, opaque ? "menu-opaque" : "menu-acrylic");
    CHECK(img.w > 0);
    const Rect box = m->panelRect(0);
    const float s = w.scale();
    // Padding rows above the first item: no text there.
    const Color left = img.at((box.x + 14) * s, (box.y + 3) * s), right = img.at((box.right() - 14) * s, (box.y + 3) * s);
    const Color fg2 = gfx::colors().fgSecondary;
    std::printf("        panel over red %.3f %.3f %.3f, over blue %.3f %.3f %.3f\n", left.r, left.g, left.b, right.r,
                right.g, right.b);
    if (opaque) {
        CHECK(std::abs(left.r - right.r) < 0.02f && std::abs(left.b - right.b) < 0.02f);
    } else {
        CHECK(left.r - right.r > 0.03f);   // the red half shows through on the left...
        CHECK(right.b - left.b > 0.03f);   // ...the blue half on the right
    }
    CHECK(contrast(left, fg2) >= 4.5f);
    CHECK(contrast(right, fg2) >= 4.5f);

    if (!opaque) {
        // Something behind changes: one recapture once it has settled, not before.
        const int c1 = Acrylic::captureCount();
        w.root()->invalidate();
        frames(w, 1);
        CHECK(Acrylic::captureCount() == c1);
        waitFrames(w, 220);
        CHECK(Acrylic::captureCount() == c1 + 1);
        frames(w, 5);
        CHECK(Acrylic::captureCount() == c1 + 1);
        // A repaint far from the menu (the corner widget, ticking for 1.5 s) never recaptures it.
        auto* ticker = static_cast<Backdrop*>(w.root())->ticker;
        const double until = frame::realNow() + 1500;
        while (frame::realNow() < until) {
            ticker->invalidate();
            Sleep(30);
            w.render();
        }
        CHECK(Acrylic::captureCount() == c1 + 1);
    }

    // Close: still painted (no input) during its animation, then gone with its backdrop.
    m->close();
    CHECK(!w.hasModal());
    CHECK(m->exiting());
    frames(w, 1);
    const Image during = shot(w, "menu-closing");
    const Color mid = during.at((box.x + 14) * s, (box.y + 3) * s);
    const Color page = during.at((box.x - 20) * s, (box.y + 3) * s);
    CHECK(std::abs(mid.r - page.r) + std::abs(mid.b - page.b) > 0.05f);   // the fading menu is still there
    waitFrames(w, 200);
    CHECK(Acrylic::liveBytes() == 0);   // its material (and the shared noise tile) released

    // Hareketi azalt: no animation, removed at once.
    motion::setReduced(true);
    m = Menu::open(&w, {300, 100}, sampleItems());
    frames(w, 1);
    m->close();
    CHECK(!m->exiting());
    frames(w, 2);
    CHECK(Acrylic::liveBytes() == 0);
    motion::setReduced(false);
}

void testToasts(Window& w) {
    std::printf("toasts\n");
    Toasts::show(&w, L"Bir");
    Toasts::show(&w, L"İki");
    Toasts::show(&w, L"Üç", ToastKind::Success);
    frames(w, 1);
    Toasts* host = Toasts::hostOf(&w);
    CHECK(host && host->count() == 3);
    if (!host) return;
    Toasts::show(&w, L"Dört: uzun bir bildirim metni, üç satıra kadar sarılır ve kutu yüksekliği buna göre büyür.", ToastKind::Error);
    frames(w, 1);
    CHECK(host->count() == 4);   // the oldest is on its way out...
    waitFrames(w, 300);
    CHECK(host->count() == 3);   // ...and gone
    CHECK(Acrylic::liveBytes() > 0);
    const Image img = shot(w, "toasts");
    CHECK(img.w > 0);
    waitFrames(w, 4400);   // they leave after their 4 s
    CHECK(host->count() == 0);
    CHECK(Acrylic::liveBytes() == 0);   // nothing on screen: the backdrop goes
}

void testDialog(Window& w) {
    std::printf("dialog\n");
    g_ran.clear();
    Dialog::confirm(&w, L"Çalma listesi silinsin mi?", L"\"Gece\" kalıcı olarak silinecek.", L"Sil", [] { g_ran = "sil"; }, true);
    frames(w, 2);
    CHECK(w.hasModal());
    key(w, VK_RETURN);   // the default button: the last one ("Sil")
    CHECK(g_ran == "sil");
    CHECK(!w.hasModal());
    waitFrames(w, 250);
    CHECK(Acrylic::liveBytes() == 0);
    g_ran.clear();
    Dialog* d = Dialog::open(&w, L"Başlık", L"Metin");
    d->addButton(L"Tamam", ButtonKind::Primary, [] { g_ran = "tamam"; });
    frames(w, 2);
    key(w, VK_ESCAPE);
    CHECK(!w.hasModal());
    CHECK(g_ran.empty());
    CHECK(d->exiting());
    waitFrames(w, 250);
    CHECK(Acrylic::liveBytes() == 0);
}

int runOpaqueChild() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SetEnvironmentVariableW(L"SHADETUBE_ACRYLIC", L"off");
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" opaque";
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) return -1;
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 60000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 2);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    SetEnvironmentVariableW(L"SHADETUBE_ACRYLIC", nullptr);
    return static_cast<int>(code);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    const bool opaque = argc > 1 && std::wstring(argv[1]) == L"opaque";
    if (!opaque) SetEnvironmentVariableW(L"SHADETUBE_ACRYLIC", L"force");   // independent of this PC's setting
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_dir = std::filesystem::temp_directory_path() / ("popups_test_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(g_dir);
    gfx::Device::get().init();
    gfx::Fonts::init();
    gfx::Theme::get().load(false);
    int childResult = 0;
    {
        WindowOptions o;
        o.title = L"popups_test";
        o.width = 800;
        o.height = 560;
        o.minWidth = 400;
        o.minHeight = 300;
        Window w(o);
        w.setRoot(std::make_unique<Backdrop>());
        frames(w, 1);
        if (opaque) {
            testExitAndAcrylic(w, true);
        } else {
            testMenuKeys(w);
            testSubmenu(w);
            testTypeAhead(w);
            testExitAndAcrylic(w, false);
            testToasts(w);
            testDialog(w);
            gfx::Theme::get().load(true);   // the light theme's material keeps the contrast too
            std::printf("(light theme)\n");
            testExitAndAcrylic(w, false);
            gfx::Theme::get().load(false);
        }
    }
    if (!opaque) {
        std::printf("opaque (child process, SHADETUBE_ACRYLIC=off)\n");
        childResult = runOpaqueChild();
        CHECK(childResult == 0);
    }
    std::error_code ec;
    std::filesystem::remove_all(g_dir, ec);
    gfx::Device::get().shutdown();
    CoUninitialize();
    std::printf(g_failures ? "%d FAILED\n" : "all passed\n", g_failures);
    return g_failures ? 1 : 0;
}
