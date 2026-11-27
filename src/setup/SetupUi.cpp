// ShadeTubeSetup.exe's window. See SetupUi.h.
#include "setup/SetupUi.h"

#include "app/Installer.h"
#include "app/Updater.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/Icons.h"
#include "gfx/Text.h"
#include "gfx/Theme.h"
#include "ui/Anim.h"
#include "ui/Controls.h"
#include "ui/Micro.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <windows.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <memory>
#include <numbers>
#include <thread>

namespace st::setup {

namespace fs = std::filesystem;
namespace installer = app::installer;
using gfx::Color;
using gfx::Rect;
using Microsoft::WRL::ComPtr;
using ui::Button;
using ui::ButtonKind;
using ui::Canvas;
namespace type = gfx::type;
namespace motion = ui::motion;

namespace {

// ---- Geometry (DIPs) ------------------------------------------------------------------------------------------
constexpr float kW = 800, kH = 620, kTitleH = 40, kLeftW = 320, kColX = 368, kPad = 40;
constexpr float kColW = kW - kColX - kPad;   // 392
constexpr float kFooterY = kH - 24 - 40;     // the footer buttons' row
constexpr float kRecordY = 318;               // the record's center (left panel)
constexpr float kRowH = 36;                   // an option row
constexpr float kMarkR = 54, kTickIn = 112, kTickOut = 121;
constexpr int kTicks = 72;                    // one every 5 degrees
constexpr uint32_t kLogoAccent = 0xDDFF47;    // the logo mark's own lime (recolored to the theme's accent)

// The big transitions (pages, the spin) follow "Hareketi azalt" and Windows' animation effects, like the app's
// micro-interactions.
bool calm() { return motion::reduced() || !motion::systemAnimations(); }
double ms(double v) { return calm() ? 0.0 : v; }

gfx::TextOptions wrapOpts(int lines) {
    gfx::TextOptions o;
    o.wrap = true;
    o.maxLines = lines;
    return o;
}

std::wstring decimal(double v) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f", v);
    std::wstring s = b;
    std::replace(s.begin(), s.end(), L'.', i18n::decimalSeparator());
    return s;
}

std::wstring sizeText(uint64_t bytes) {
    const double mb = static_cast<double>(bytes) / 1048576.0;
    if (mb < 1024) return i18n::format(tr(L"{} MB"), {decimal(mb)});
    return i18n::format(tr(L"{} GB"), {decimal(mb / 1024.0)});
}

// "C:\Users\…\Programs\ShadeTube": components after the first are dropped from the middle until it fits.
std::wstring fitPath(const std::wstring& path, const gfx::TextStyle& style, float maxW) {
    auto width = [&](const std::wstring& s) { return gfx::Text(s, style).measure().w; };
    if (width(path) <= maxW) return path;
    std::vector<std::wstring> parts;
    for (size_t start = 0;;) {
        const size_t sep = path.find(L'\\', start);
        parts.push_back(path.substr(start, sep == std::wstring::npos ? std::wstring::npos : sep - start));
        if (sep == std::wstring::npos) break;
        start = sep + 1;
    }
    if (parts.size() < 3) return path;
    for (size_t drop = 1; drop + 1 < parts.size(); ++drop) {
        std::wstring s = parts[0] + L"\\…";
        for (size_t i = 1 + drop; i < parts.size(); ++i) s += L"\\" + parts[i];
        if (width(s) <= maxW || drop + 2 == parts.size()) return s;
    }
    return path;
}

// The percentage in the UI language's order ("%62", "62 %"): prefix, number, suffix around the "{}".
struct PercentParts {
    std::wstring prefix, suffix;
};
PercentParts percentParts() {
    const std::wstring p = tr(L"%{}");
    const size_t at = p.find(L"{}");
    if (at == std::wstring::npos) return {L"", L"%"};
    return {p.substr(0, at), p.substr(at + 2)};
}

// Two hero lines ("Müziğin," bold / "bir tık uzağında." light): one translated text, split at its line break, set at
// the largest size at which both lines fit the column.
struct Hero {
    gfx::Text a, b;
    float size = 40;
    void set(const std::wstring& text) {
        const size_t br = text.find(L'\n');
        a.setText(br == std::wstring::npos ? text : text.substr(0, br));
        b.setText(br == std::wstring::npos ? std::wstring() : text.substr(br + 1));
    }
    float lineH() const { return std::ceil(size * 1.12f); }
    float fit(float w, float maxSize = 40) {
        for (const float s : {40.f, 36.f, 32.f, 28.f, 24.f}) {
            if (s > maxSize) continue;
            size = s;
            a.setStyle(type::displayL.withSize(s));
            b.setStyle(type::displayL.withSize(s).withWeight(300));
            if (std::max(a.measure().w, b.measure().w) <= w) break;
        }
        return lineH() * (b.empty() ? 1 : 2);
    }
    void paint(Canvas& c, float x, float y, float w) {
        const auto& col = gfx::colors();
        c.text(a, {x - 2, y, w + 4, lineH()}, col.fgPrimary);
        if (!b.empty()) c.text(b, {x - 2, y + lineH(), w + 4, lineH()}, col.fgPrimary);
    }
};

void monoLabel(Canvas& c, const std::wstring& text, float x, float y, float w, Color color) {
    c.text(text, type::monoLabel, {x, y, w, 14}, color);
}

// ---- Model -----------------------------------------------------------------------------------------------------

enum class PageId { Welcome, Maintenance, Uninstall, Progress, Done, Error };

struct Model {
    Args args;
    std::string_view payload;
    payload::Info info;
    std::wstring payloadError;
    Existing existing;
    Action action = Action::Install;   // what "Kur / Güncelle / Onar" does
    fs::path dir;
    bool dirOk = true;
    int64_t freeBytes = -1;
    bool startMenu = true, desktop = true, autostart = false, launchWhenDone = true;
    bool keepData = true;
    // The job on the progress page and its outcome.
    Action job = Action::Install;
    bool jobHadRunning = false;
    Result result;
    bool demo = false;   // screenshots: nothing runs
};

class Ui;

// ---- Widgets ---------------------------------------------------------------------------------------------------

// An option: label (and an optional second line) with a toggle at the right end; the whole row flips it.
class OptionRow : public ui::Widget {
public:
    OptionRow(std::wstring title, std::wstring desc, bool on, std::function<void(bool)> changed)
        : title_(std::move(title), type::body.withWeight(500)), desc_(std::move(desc), type::secondary, wrapOpts(3)) {
        toggle_ = add<ui::Toggle>();
        toggle_->setOn(on, false);
        toggle_->onChange = std::move(changed);
    }
    bool last = false;
    float rowH = kRowH;   // a row without a second line (the welcome page tightens it when a language needs the room)
    float preferredHeight(float w) override {
        if (desc_.empty()) return rowH;
        return std::ceil(10 + 20 + 2 + desc_.measure(w - 64).h + 10);
    }
    void layout() override { toggle_->setRect({rect().w - 40, std::floor(rect().h * 0.5f - 11), 40, 22}); }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = gfx::colors();
        const float tw = r.w - 64;
        if (desc_.empty()) {
            c.text(title_, {r.x, r.y, tw, r.h}, col.fgPrimary, gfx::VAlign::Center);
        } else {
            c.text(title_, {r.x, r.y + 10, tw, 20}, col.fgPrimary);
            c.text(desc_, {r.x, r.y + 32, tw, r.h - 32}, col.fgSecondary);
        }
        if (!last) c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
        paintChildren(c);
    }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left && enabled(); }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (hovered() && rect().contains(e.pos)) toggle_->onActivate();
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    ui::Toggle* toggle() const { return toggle_; }

private:
    gfx::Text title_, desc_;
    ui::Toggle* toggle_ = nullptr;
};

// The install folder: a read-only field with the folder glyph (the path shortened in the middle when long).
void paintFolder(Canvas& c, const Rect& r, const fs::path& dir, bool error) {
    const auto& col = gfx::colors();
    c.fillRounded(r, gfx::radii().field, col.bgSunken);
    c.strokeRounded(r, gfx::radii().field, error ? col.error.withAlpha(0.7f) : col.hairControl);
    c.icon("folder", {r.x + 12, r.cy() - 8, 16, 16}, error ? col.error : col.fgTertiary);
    const auto style = type::secondary;
    const float tw = r.w - 12 - 16 - 10 - 12;
    c.text(fitPath(dir.wstring(), style, tw), style, {r.x + 38, r.y, tw, r.h}, col.fgPrimary, gfx::TextAlign::Leading,
           gfx::VAlign::Center);
}

// A line of note under a control: an icon in its color and the text (wraps to two lines).
float paintNote(Canvas& c, float x, float y, float w, const char* icon, Color iconColor, const std::wstring& text, Color color) {
    gfx::Text t(text, type::caption, wrapOpts(2));
    const float h = std::max(16.f, std::ceil(t.measure(w - 22).h));
    c.icon(icon, {x, y + 1, 14, 14}, iconColor);
    c.text(t, {x + 22, y, w - 22, h + 2}, color);
    return h;
}

// ---- Pages -----------------------------------------------------------------------------------------------------

class Page : public ui::Widget {
public:
    explicit Page(Ui& ui) : ui_(ui) { hitTestVisible = false; }
    void enter(bool animate, float dir) {
        dir_ = dir;
        appear_.snap(animate && !calm() ? 0.f : 1.f);
        appear_.to(1, ms(motion::medium), ui::Ease::Decelerate);
    }
    void leave() {
        leaving_ = true;
        for (auto& ch : children()) ch->setEnabled(false);   // a key or click never reaches a page on its way out
        appear_.to(0, ms(motion::fast), ui::Ease::Accelerate);
    }
    bool leaving() const { return leaving_; }
    bool gone() const { return leaving_ && !appear_.running(); }
    Widget* hitTest(gfx::Point p) override { return leaving_ ? nullptr : Widget::hitTest(p); }
    void paint(Canvas& c) final {
        const float t = appear_.value();
        if (t <= 0.001f) return;
        const bool layer = t < 0.999f;
        if (layer) {
            c.pushOpacity(t);
            c.pushTransform(D2D1::Matrix3x2F::Translation((1 - t) * 20.f * dir_, 0));
        }
        paintPage(c);
        paintChildren(c);
        if (layer) {
            c.popTransform();
            c.popLayer();
        }
    }
    virtual void paintPage(Canvas&) {}
    virtual void primary() {}   // Enter
    virtual void escape() {}    // Esc
    virtual Widget* initialFocus() { return nullptr; }

protected:
    Ui& ui_;
    ui::Anim appear_{1};
    float dir_ = 1;
    bool leaving_ = false;
};

// ---- The window's root: background, record, title bar, the pages -------------------------------------------------

class Root : public ui::Widget {
public:
    explicit Root(Model& m) : m_(m) {
        isDragRegion = true;
        min_ = add<Button>(ButtonKind::Caption, L"", "minimize");
        close_ = add<Button>(ButtonKind::CaptionClose, L"", "close");
    }
    Button* min_ = nullptr;
    Button* close_ = nullptr;

    // The record's state.
    ui::Anim shown;        // the progress the ticks / percentage show (eases towards the real one)
    ui::Anim spinSpeed;    // revolutions per second
    ui::Anim badge;        // 0..1: the done (check) / failed (error) badge
    bool failed = false;
    float angle = 0;       // degrees
    double lastSpin = 0;

    void layout() override {
        min_->setRect({kW - 8 - 80, 6, 40, 28});
        close_->setRect({kW - 8 - 40, 6, 40, 28});
        for (auto& ch : children())
            if (dynamic_cast<Page*>(ch.get())) ch->setRect({0, 0, kW, kH});
    }

    void paint(Canvas& c) override {
        const auto& col = gfx::colors();
        const auto& acc = gfx::accent();
        // Left panel: a sunken surface with the accent's glow behind the record.
        const Rect left{0, 0, kLeftW, kH};
        c.fillRect(left, col.bgSunken);
        const gfx::Point center{kLeftW * 0.5f, kRecordY};
        c.fillRadialGradient(left, center, 230, acc.base.withAlpha(gfx::isLightTheme() ? 0.16f : 0.09f), acc.base.withAlpha(0));
        c.vline(kLeftW, 0, kH, col.hairSubtle);
        paintRecord(c, center);

        // Title bar: the mark in the accent, the name, "KURULUM" (like the app's own title bar).
        gfx::Icons::drawRecolored(c.dc(), "logo/logo-mark", {16, 11, 18, 18}, c.scale(), Color::rgb(kLogoAccent), acc.base);
        c.text(L"ShadeTube", type::body.withSize(13).withWeight(700), {42, 0, 120, kTitleH}, col.fgPrimary, gfx::TextAlign::Leading,
               gfx::VAlign::Center);
        const float nameW = std::ceil(gfx::Text(L"ShadeTube", type::body.withSize(13).withWeight(700)).measure().w);
        c.text(toUpperTr(tr(L"Kurulum")), type::monoLabel, {42 + nameW + 12, 0, 200, kTitleH}, col.fgTertiary, gfx::TextAlign::Leading,
               gfx::VAlign::Center);

        // Bottom of the left panel: the version and what this package is.
        monoLabel(c, toUpperTr(i18n::format(tr(L"Sürüm {}"), {toWide(m_.info.version)})), 40, kH - 58, kLeftW - 60, col.fgTertiary);
        monoLabel(c, L"WINDOWS · X64 · " + toUpperTr(sizeText(m_.info.totalSize)), 40, kH - 40, kLeftW - 60, col.fgTertiary);

        // Pages (the leaving one under the new one), then the caption buttons on top.
        for (auto& ch : children())
            if (auto* p = dynamic_cast<Page*>(ch.get()); p && p->leaving()) p->paint(c);
        for (auto& ch : children())
            if (auto* p = dynamic_cast<Page*>(ch.get()); p && !p->leaving()) p->paint(c);
        min_->paint(c);
        close_->paint(c);
    }

private:
    void paintRecord(Canvas& c, gfx::Point o) {
        const auto& col = gfx::colors();
        const auto& acc = gfx::accent();
        // Two thin rings, then the dial: ticks every 5 degrees, lit clockwise from the top with the progress.
        c.strokeCircle(o, kMarkR + 22, col.hairSubtle);
        c.strokeCircle(o, kMarkR + 44, col.hairSubtle);
        const float p = std::clamp(shown.value(), 0.f, 1.f) * kTicks;
        for (int i = 0; i < kTicks; ++i) {
            const float a = (static_cast<float>(i) * 360.f / kTicks - 90.f) * std::numbers::pi_v<float> / 180.f;
            const float lit = std::clamp(p - static_cast<float>(i), 0.f, 1.f);
            const bool major = i % 6 == 0;
            const float in = major ? kTickIn - 4 : kTickIn;
            const Color base = major ? col.hairStrong : col.hairDefault;
            const Color tint = failed ? col.error : acc.base;
            c.line({o.x + std::cos(a) * in, o.y + std::sin(a) * in}, {o.x + std::cos(a) * kTickOut, o.y + std::sin(a) * kTickOut},
                   Color::lerp(base, tint, lit), major ? 1.5f : 1.25f, true);
        }
        // The mark, spinning like a record while the files are written.
        const double now = ui::frame::now();
        const float speed = spinSpeed.value();
        if (lastSpin > 0 && speed > 0.0005f) angle = std::fmod(angle + static_cast<float>((now - lastSpin) / 1000.0) * speed * 360.f, 360.f);
        lastSpin = now;
        if (speed > 0.0005f) ui::frame::requestNext();
        c.pushTransform(D2D1::Matrix3x2F::Rotation(angle, {o.x, o.y}));
        gfx::Icons::drawRecolored(c.dc(), "logo/logo-mark", {o.x - kMarkR, o.y - kMarkR, kMarkR * 2, kMarkR * 2}, c.scale(),
                                  Color::rgb(kLogoAccent), acc.base);
        c.popTransform();
        // The badge: done (a check on the accent) or failed (the error color).
        if (const float b = badge.value(); b > 0.001f) {
            const gfx::Point bc{o.x + kMarkR * 0.72f, o.y + kMarkR * 0.72f};
            const float r = 17 * (0.6f + 0.4f * b);
            c.pushOpacity(std::min(1.f, b * 1.4f));
            c.fillCircle(bc, r + 3, col.bgSunken);
            c.fillCircle(bc, r, failed ? col.error : acc.base);
            c.icon(failed ? "close" : "check", {bc.x - 9, bc.y - 9, 18, 18}, failed ? Color::rgb(0xFFFFFF) : acc.onAccent);
            c.popLayer();
        }
    }
    Model& m_;
};

// ---- The controller ------------------------------------------------------------------------------------------------

class Ui {
public:
    Ui(Model& m, ui::Window& w) : m(m), window(w) {
        auto root = std::make_unique<Root>(m);
        root_ = root.get();
        window.setRoot(std::move(root));
        root_->min_->onClick = [this] { window.minimize(); };
        root_->close_->onClick = [this] { requestClose(); };
        window.onCloseRequested = [this] { requestClose(); };
        window.onKey = [this](const ui::KeyEvent& e) {
            if (!page_ || e.ctrl || e.alt) return false;
            if (e.vk == VK_RETURN && !e.repeat) {
                page_->primary();
                return true;
            }
            if (e.vk == VK_ESCAPE) {
                page_->escape();
                return true;
            }
            return false;
        };
    }
    ~Ui() {
        if (worker_.joinable()) worker_.join();
    }

    Model& m;
    ui::Window& window;
    int exitCode = kExitCancelled;
    bool closing = false;

    Root& root() { return *root_; }
    PageId pageId() const { return pageId_; }
    bool busy() const { return pageId_ == PageId::Progress; }

    void show(PageId id, bool animate = true);
    void startJob(Action job);
    void onProgress(Step step, float overall);
    void requestClose() {
        if (busy()) return;   // the files are being written: never half way
        closing = true;
        DestroyWindow(window.hwnd());
    }
    // Main loop housekeeping: the progress page hands over to the done / error page once the shown progress has
    // caught up and it has been on screen long enough to read; the done page may close the window by itself.
    void update();
    double nextWake() const;
    Step step() const { return step_; }
    void setStep(Step s) { step_ = s; }
    void setFinished(Result r) {
        m.result = std::move(r);
        finished_ = true;
    }
    void pickFolder();
    void taskbar(float overall, bool error = false);

private:
    Root* root_ = nullptr;
    Page* page_ = nullptr;
    PageId pageId_ = PageId::Welcome;
    std::thread worker_;
    Step step_ = Step::Verify;
    bool finished_ = false;
    double progressSince_ = 0;
    double autoCloseAt_ = 0;
    ComPtr<ITaskbarList3> taskbar_;
};

// ---- Welcome / maintenance -------------------------------------------------------------------------------------

class WelcomePage : public Page {
public:
    explicit WelcomePage(Ui& ui) : Page(ui) {
        Model& m = ui.m;
        const bool fresh = !m.existing.installed;
        const std::wstring setupV = toWide(m.info.version), installedV = toWide(m.existing.version);
        switch (m.action) {
        case Action::Update:
            label_ = tr(L"Güncelleme");
            hero_.set(tr(L"Yeni sürüm\nhazır."));
            sub_ = i18n::format(tr(L"Kurulu sürüm {}, bu kurulum {}. Ayarların, oturumun, kitaplığın ve çalma sıran aynen kalır."),
                                {installedV, setupV});
            break;
        case Action::Repair:
            label_ = tr(L"Kurulu");
            hero_.set(tr(L"ShadeTube\nzaten kurulu."));
            sub_ = m.existing.exeMissing
                       ? tr(L"Kayıt duruyor ama program dosyası eksik. Onarmak dosyaları ve kısayolları yeniden yerine koyar.")
                       : i18n::format(tr(L"ShadeTube {} bu bilgisayarda kurulu. Onarmak dosyaları ve kısayolları yeniden yazar; ayarların "
                                         L"kalır."),
                                      {setupV});
            break;
        case Action::Downgrade:
            label_ = tr(L"Kurulu");
            hero_.set(tr(L"Daha yeni bir\nsürüm kurulu."));
            sub_ = i18n::format(tr(L"Kurulu sürüm {}, bu kurulum {}. Yine de kurarsan eski sürüm yerine geçer."), {installedV, setupV});
            break;
        default:
            label_ = tr(L"Hoş geldin");
            hero_.set(tr(L"Müziğin,\nbir tık uzağında."));
            sub_ = tr(L"Reklamsız, hafif, yerel bir müzik çalar. Spotify kitaplığın burada, sesler YouTube'dan akar.");
            break;
        }
        subText_ = gfx::Text(sub_, type::bodyL.withSize(14), wrapOpts(3));

        if (fresh) {
            change_ = add<Button>(ButtonKind::Secondary, tr(L"Değiştir…"), "folder");
            change_->onClick = [&ui] { ui.pickFolder(); };
        }
        rows_.push_back(add<OptionRow>(tr(L"Başlat menüsü kısayolu"), L"", m.startMenu, [&m](bool v) { m.startMenu = v; }));
        rows_.push_back(add<OptionRow>(tr(L"Masaüstü kısayolu"), L"", m.desktop, [&m](bool v) { m.desktop = v; }));
        rows_.push_back(add<OptionRow>(tr(L"Windows ile başlat"), L"", m.autostart, [&m](bool v) { m.autostart = v; }));
        rows_.push_back(add<OptionRow>(tr(L"Bitince ShadeTube'u aç"), L"", m.launchWhenDone, [&m](bool v) { m.launchWhenDone = v; }));
        rows_.back()->last = true;

        if (!fresh) {
            remove_ = add<Button>(ButtonKind::Ghost, tr(L"Kaldır…"), "trash");
            remove_->setDestructive(true);
            remove_->onClick = [&ui] { ui.show(PageId::Uninstall); };
        }
        cancel_ = add<Button>(ButtonKind::Ghost, tr(L"Vazgeç"));
        cancel_->onClick = [&ui] { ui.requestClose(); };
        const wchar_t* go = m.action == Action::Update      ? tr(L"Güncelle")
                            : m.action == Action::Repair    ? tr(L"Onar")
                            : m.action == Action::Downgrade ? tr(L"Yine de kur")
                                                            : tr(L"Kur");
        go_ = add<Button>(m.action == Action::Downgrade ? ButtonKind::Secondary : ButtonKind::Primary, go,
                          m.action == Action::Install ? "download" : "refresh");
        go_->onClick = [this] { primary(); };
        refresh();
    }

    // The folder changed (picker): its note and "Kur".
    void refresh() {
        go_->setEnabled(ui_.m.dirOk);
        requestLayout();
        invalidate();
    }

    void layout() override {
        // Top to bottom: label, hero, subtitle, folder, its note, the options. A language that needs more room (a
        // three-line subtitle, a two-line note) gets a smaller hero, tighter gaps and rows, until the options end above
        // the footer.
        const float subH = std::ceil(subText_.measure(kColW).h);
        const float noteH = std::max(16.f, std::ceil(gfx::Text(noteText(), type::caption, wrapOpts(2)).measure(kColW - 22).h));
        struct Fit {
            float hero, gap, row;
        };
        for (const Fit f : {Fit{40, 1.f, kRowH}, Fit{36, 0.75f, 34}, Fit{32, 0.6f, 32}, Fit{28, 0.5f, 30}}) {
            y_ = 72;
            heroH_ = hero_.fit(kColW, f.hero);
            subY_ = y_ + 24 + heroH_ + std::round(14 * f.gap);
            folderY_ = subY_ + subH + std::round(26 * f.gap);
            noteY_ = folderY_ + 22 + 40 + 8;
            float y = noteY_ + noteH + std::round(12 * f.gap);
            for (auto* r : rows_) {
                r->rowH = f.row;
                y += r->preferredHeight(kColW);
            }
            if (y <= kFooterY - 12) break;
        }
        const float fieldY = folderY_ + 22;
        float fieldW = kColW;
        if (change_) {
            const float bw = std::ceil(change_->naturalWidth());
            change_->setRect({kColX + kColW - bw, fieldY, bw, 40});
            fieldW = kColW - bw - 8;
        }
        field_ = {kColX, fieldY, fieldW, 40};
        float y = noteY_ + noteH + (rows_.front()->rowH < kRowH ? 8.f : 12.f);
        for (auto* r : rows_) {
            const float h = r->preferredHeight(kColW);
            r->setRect({kColX, y, kColW, h});
            y += h;
        }
        float x = kColX + kColW;
        for (Button* b : {go_, cancel_}) {
            const float bw = std::ceil(b->naturalWidth());
            x -= bw;
            b->setRect({x, kFooterY, bw, 40});
            x -= 8;
        }
        if (remove_) remove_->setRect({kColX - 12, kFooterY, std::ceil(remove_->naturalWidth()), 40});
    }

    void paintPage(Canvas& c) override {
        const auto& col = gfx::colors();
        const auto& acc = gfx::accent();
        const Model& m = ui_.m;
        monoLabel(c, toUpperTr(label_), kColX, y_, kColW, acc.base);
        hero_.paint(c, kColX, y_ + 24, kColW);
        c.text(subText_, {kColX, subY_, kColW, 90}, col.fgSecondary);
        monoLabel(c, toUpperTr(tr(L"Kurulum klasörü")), kColX, folderY_, kColW, col.fgTertiary);
        paintFolder(c, field_, m.dir, !m.dirOk);
        // Under the field, one note: why the folder can't be used, else that ShadeTube is open, else the room it takes.
        if (!m.dirOk) paintNote(c, kColX, noteY_, kColW, "error", col.error, noteText(), col.error);
        else if (!m.existing.running.empty()) paintNote(c, kColX, noteY_, kColW, "warning", acc.base, noteText(), col.fgSecondary);
        else c.text(noteText(), type::caption, {kColX, noteY_, kColW, 16}, col.fgTertiary);
    }

    std::wstring noteText() const {
        const Model& m = ui_.m;
        if (!m.dirOk) return tr(L"Bu klasör yönetici izni istiyor: kendi klasörlerinden birini seç.");
        if (!m.existing.running.empty()) return tr(L"ShadeTube açık: devam edince kapatılır, hiçbir şey kaybolmaz.");
        std::wstring note = i18n::format(tr(L"{} gerekir"), {sizeText(m.info.totalSize)});
        if (m.freeBytes >= 0) note += L" · " + i18n::format(tr(L"{} boş"), {sizeText(static_cast<uint64_t>(m.freeBytes))});
        return note + L" · " + tr(L"yönetici izni gerekmez");
    }

    void primary() override {
        if (!go_->enabled()) return;
        ui_.startJob(ui_.m.action);
    }
    void escape() override { ui_.requestClose(); }
    Widget* initialFocus() override { return go_; }

private:
    std::wstring label_, sub_;
    Hero hero_;
    gfx::Text subText_;
    Button *change_ = nullptr, *remove_ = nullptr, *cancel_ = nullptr, *go_ = nullptr;
    std::vector<OptionRow*> rows_;
    float y_ = 0, heroH_ = 0, subY_ = 0, folderY_ = 0, noteY_ = 0;
    Rect field_{};
};

// ---- Uninstall -------------------------------------------------------------------------------------------------

class UninstallPage : public Page {
public:
    explicit UninstallPage(Ui& ui) : Page(ui) {
        Model& m = ui.m;
        hero_.set(tr(L"ShadeTube\nkaldırılsın mı?"));
        sub_ = gfx::Text(tr(L"Program dosyaları, kısayollar ve Yüklü uygulamalar kaydı silinir. İndirdiğin şarkılar silinmez."),
                         type::bodyL.withSize(14), wrapOpts(3));
        keep_ = add<OptionRow>(tr(L"Ayarlarımı ve kitaplığımı sakla"),
                               tr(L"Spotify oturumu, kitaplık, ayarlar ve önbellek bu bilgisayarda kalır; yeniden kurunca her şey yerinde olur."),
                               m.keepData, [&m](bool v) { m.keepData = v; });
        keep_->last = true;
        back_ = add<Button>(ButtonKind::Ghost, tr(L"Vazgeç"));
        back_->onClick = [this] { escape(); };
        go_ = add<Button>(ButtonKind::Secondary, tr(L"Kaldır"), "trash");
        go_->setDestructive(true);
        go_->onClick = [this] { primary(); };
    }
    void layout() override {
        heroH_ = hero_.fit(kColW);
        subY_ = 72 + 24 + heroH_ + 14;
        const float subH = std::ceil(sub_.measure(kColW).h);
        folderY_ = subY_ + subH + 26;
        field_ = {kColX, folderY_ + 22, kColW, 40};
        const float h = keep_->preferredHeight(kColW);
        keep_->setRect({kColX, field_.bottom() + 22, kColW, h});
        runningY_ = keep_->rect().bottom() + 12;
        float x = kColX + kColW;
        for (Button* b : {go_, back_}) {
            const float bw = std::ceil(b->naturalWidth());
            x -= bw;
            b->setRect({x, kFooterY, bw, 40});
            x -= 8;
        }
    }
    void paintPage(Canvas& c) override {
        const auto& col = gfx::colors();
        monoLabel(c, toUpperTr(tr(L"Kaldırma")), kColX, 72, kColW, col.error);
        hero_.paint(c, kColX, 96, kColW);
        c.text(sub_, {kColX, subY_, kColW, 80}, col.fgSecondary);
        monoLabel(c, toUpperTr(tr(L"Konum")), kColX, folderY_, kColW, col.fgTertiary);
        paintFolder(c, field_, ui_.m.existing.dir, false);
        if (!ui_.m.existing.running.empty())
            paintNote(c, kColX, runningY_, kColW, "warning", gfx::accent().base,
                      tr(L"ShadeTube açık: kaldırmadan önce kapatılır."), col.fgSecondary);
    }
    void primary() override { ui_.startJob(Action::Uninstall); }
    void escape() override { ui_.show(PageId::Maintenance); }
    Widget* initialFocus() override { return back_; }   // the safe choice first

private:
    Hero hero_;
    gfx::Text sub_;
    OptionRow* keep_ = nullptr;
    Button *back_ = nullptr, *go_ = nullptr;
    float heroH_ = 0, subY_ = 0, folderY_ = 0, runningY_ = 0;
    Rect field_{};
};

// ---- Progress --------------------------------------------------------------------------------------------------

class ProgressPage : public Page {
public:
    explicit ProgressPage(Ui& ui) : Page(ui) {
        const Model& m = ui.m;
        const bool removing = m.job == Action::Uninstall;
        label_ = removing ? tr(L"Kaldırılıyor") : m.job == Action::Update ? tr(L"Güncelleniyor") : tr(L"Kuruluyor");
        if (removing) {
            if (m.jobHadRunning) steps_.push_back({tr(L"Açık ShadeTube kapatılıyor"), Step::Close});
            steps_.push_back({tr(L"Dosyalar, kısayollar ve kayıt siliniyor"), Step::Remove});
        } else {
            steps_.push_back({tr(L"Paket doğrulanıyor"), Step::Verify});
            if (m.jobHadRunning) steps_.push_back({tr(L"Açık ShadeTube kapatılıyor"), Step::Close});
            steps_.push_back({tr(L"Dosyalar açılıyor ve yerleştiriliyor"), Step::Unpack});
            steps_.push_back({tr(L"Kısayollar ve Windows kaydı"), Step::Register});
        }
        parts_ = percentParts();
    }
    void paintPage(Canvas& c) override {
        const auto& col = gfx::colors();
        const auto& acc = gfx::accent();
        const float p = std::clamp(ui_.root().shown.value(), 0.f, 1.f);
        monoLabel(c, toUpperTr(label_), kColX, 72, kColW, acc.base);
        // The percentage, display size: the number bold, the sign light (its side follows the language).
        const auto big = type::displayL.withSize(72);
        const auto sign = type::displayL.withSize(40).withWeight(300);
        const std::wstring num = std::to_wstring(static_cast<int>(std::floor(p * 100.f + 0.0001f)));
        float x = kColX - 3;
        const float base = 112;   // top of the number's box
        if (!parts_.prefix.empty()) {
            const float w = std::ceil(gfx::Text(parts_.prefix, sign).measure().w);
            c.text(parts_.prefix, sign, {x, base + 22, w + 4, 50}, col.fgSecondary);
            x += w + 2;
        }
        const float nw = std::ceil(gfx::Text(num, big).measure().w);
        c.text(num, big, {x, base, nw + 6, 84}, col.fgPrimary);
        x += nw + 4;
        if (!parts_.suffix.empty()) c.text(parts_.suffix, sign, {x, base + 22, 80, 50}, col.fgSecondary);

        // The player bar's wave line: the done part flows in the accent, the playhead rides its end.
        ui::WaveLine s;
        s.value = p;
        s.amp = calm() ? 0.f : ui::kWaveAmp;
        s.phase = ui::wavePhase(ui::frame::now());
        s.stroke = s.restStroke = 1.75f;
        s.gap = 4.5f;
        s.played = acc.base;
        s.rest = col.hairStrong;
        s.bufferedColor = col.hairStrong;
        const float wy = 220;
        ui::drawWaveLine(c, kColX, wy, kColW, s);
        c.fillPill({kColX + kColW * p - 2, wy - 8, 4, 16}, acc.base);
        if (!calm()) window()->invalidateAfterShown(ui::kWaveFrameMs);

        // The steps: done (a check on the accent), current (a ring turning), still to come (a hairline ring).
        const int current = currentIndex();
        float y = 262;
        for (size_t i = 0; i < steps_.size(); ++i) {
            const gfx::Point o{kColX + 10, y + 12};
            const bool done = static_cast<int>(i) < current, now = static_cast<int>(i) == current;
            if (done) {
                c.fillCircle(o, 10, acc.base);
                c.icon("check", {o.x - 7, o.y - 7, 14, 14}, acc.onAccent);
            } else if (now) {
                c.strokeCircle(o, 9.5f, col.hairDefault, 1.5f);
                const float spin = calm() ? 0.f : static_cast<float>(std::fmod(ui::frame::now() / 900.0, 1.0) * 360.0);
                c.arc(o, 9.5f, spin, 100, acc.base, 1.75f);
            } else {
                c.strokeCircle(o, 9.5f, col.hairDefault, 1.25f);
            }
            c.text(steps_[i].text, type::body.withWeight(done || now ? 600.f : 400.f), {kColX + 32, y, kColW - 32, 24},
                   now ? col.fgPrimary : done ? col.fgSecondary : col.fgTertiary, gfx::TextAlign::Leading, gfx::VAlign::Center);
            y += 36;
        }
        c.text(tr(L"Birkaç saniye sürer. Pencereyi kapatma."), type::caption, {kColX, kFooterY + 12, kColW, 16}, col.fgTertiary);
    }

private:
    struct Item {
        std::wstring text;
        Step from;
    };
    int currentIndex() const {
        const Step s = ui_.step();
        if (s == Step::Done) return static_cast<int>(steps_.size());
        auto rank = [](Step x) {
            switch (x) {
            case Step::Verify: return 0;
            case Step::Close: return 1;
            case Step::Unpack: case Step::Copy: return 2;
            case Step::Register: return 3;
            case Step::Remove: return 2;
            default: return 9;
            }
        };
        int idx = 0;
        for (size_t i = 0; i < steps_.size(); ++i)
            if (rank(steps_[i].from) <= rank(s)) idx = static_cast<int>(i);
        return idx;
    }
    std::wstring label_;
    std::vector<Item> steps_;
    PercentParts parts_;
};

// ---- Done / error ----------------------------------------------------------------------------------------------

class DonePage : public Page {
public:
    explicit DonePage(Ui& ui, bool error) : Page(ui), error_(error) {
        const Model& m = ui.m;
        const bool removed = m.job == Action::Uninstall;
        const std::wstring v = toWide(m.info.version);
        std::wstring sub;
        if (error) {
            label_ = tr(L"Olmadı");
            hero_.set(removed ? tr(L"Kaldırma\ntamamlanamadı.") : tr(L"Kurulum\ntamamlanamadı."));
            sub = m.payloadError.empty() ? m.result.error : m.payloadError;
            if (sub.empty()) sub = tr(L"Beklenmeyen bir hata oldu.");
        } else if (removed) {
            label_ = tr(L"Tamamlandı");
            hero_.set(tr(L"Kaldırıldı.\nGörüşmek üzere."));
            sub = m.result.dataKept ? tr(L"Program kaldırıldı; açık bir ShadeTube kapatılamadığı için ayarlar ve veriler silinmedi.")
                  : m.keepData      ? tr(L"Ayarların, oturumun ve kitaplığın bu bilgisayarda duruyor: yeniden kurunca kaldığın yerden devam edersin.")
                                    : tr(L"Ayarlar, oturum, kitaplık ve önbellek de silindi. İndirdiğin şarkılar yerinde.");
        } else {
            label_ = tr(L"Tamamlandı");
            hero_.set(m.job == Action::Update ? tr(L"Güncellendi.\nİyi dinlemeler.") : tr(L"Hazır.\nİyi dinlemeler."));
            sub = m.job == Action::Update ? i18n::format(tr(L"ShadeTube {} sürümüne güncellendi"), {v})
                                          : i18n::format(tr(L"ShadeTube {} bu bilgisayara kuruldu. Windows Ayarlar › Uygulamalar'dan istediğin zaman kaldırabilirsin."), {v});
        }
        sub_ = gfx::Text(sub, type::bodyL.withSize(14), wrapOpts(5));
        autoLaunch_ = !error && !removed && m.launchWhenDone;
        if (autoLaunch_) {
            close_ = nullptr;
        } else if (error) {
            close_ = add<Button>(ButtonKind::Ghost, tr(L"Kapat"));
            close_->onClick = [&ui] { ui.requestClose(); };
            if (m.payloadError.empty()) {
                retry_ = add<Button>(ButtonKind::Primary, tr(L"Tekrar dene"), "refresh");
                retry_->onClick = [this] { primary(); };
            }
        } else if (removed) {
            close_ = add<Button>(ButtonKind::Primary, tr(L"Kapat"));
            close_->onClick = [&ui] { ui.requestClose(); };
        } else {
            close_ = add<Button>(ButtonKind::Ghost, tr(L"Kapat"));
            close_->onClick = [&ui] { ui.requestClose(); };
            open_ = add<Button>(ButtonKind::Primary, tr(L"ShadeTube'u aç"), "play");
            open_->onClick = [this] { primary(); };
        }
    }
    void layout() override {
        heroH_ = hero_.fit(kColW);
        subY_ = 72 + 24 + heroH_ + 14;
        float x = kColX + kColW;
        for (Button* b : {retry_, open_, close_}) {
            if (!b) continue;
            const float bw = std::ceil(b->naturalWidth());
            x -= bw;
            b->setRect({x, kFooterY, bw, 40});
            x -= 8;
        }
    }
    void paintPage(Canvas& c) override {
        const auto& col = gfx::colors();
        const Model& m = ui_.m;
        monoLabel(c, toUpperTr(label_), kColX, 72, kColW, error_ ? col.error : gfx::accent().base);
        hero_.paint(c, kColX, 96, kColW);
        const float subH = std::ceil(sub_.measure(kColW).h);
        c.text(sub_, {kColX, subY_, kColW, subH + 4}, col.fgSecondary);
        float y = subY_ + subH + 26;
        if (!error_ && m.job != Action::Uninstall) {
            monoLabel(c, toUpperTr(tr(L"Kurulum klasörü")), kColX, y, kColW, col.fgTertiary);
            paintFolder(c, {kColX, y + 22, kColW, 40}, m.result.exe.empty() ? m.dir : m.result.exe.parent_path(), false);
            y += 22 + 40 + 16;
            if (m.result.stillRunning)
                y += 8 + paintNote(c, kColX, y, kColW, "info", gfx::accent().base,
                                   tr(L"Açık kalan ShadeTube yeniden başlatılınca yeni sürüm açılır."), col.fgSecondary);
        }
        if (autoLaunch_)
            c.text(tr(L"ShadeTube açılıyor…"), type::body.withWeight(500), {kColX, kFooterY, kColW, 40}, col.fgSecondary,
                   gfx::TextAlign::Leading, gfx::VAlign::Center);
    }
    void primary() override {
        if (error_) {
            if (retry_) ui_.show(ui_.m.existing.installed ? PageId::Maintenance : PageId::Welcome);
            else ui_.requestClose();
            return;
        }
        if (open_) {
            const auto exe = ui_.m.result.exe;
            if (!ui_.m.demo && !exe.empty()) installer::launch(exe, ui_.m.job == Action::Update ? L"--updated" : L"--installed");
        }
        ui_.requestClose();
    }
    void escape() override { ui_.requestClose(); }
    Widget* initialFocus() override { return retry_ ? retry_ : open_ ? open_ : close_; }

private:
    bool error_;
    bool autoLaunch_ = false;
    std::wstring label_;
    Hero hero_;
    gfx::Text sub_;
    Button *close_ = nullptr, *retry_ = nullptr, *open_ = nullptr;
    float heroH_ = 0, subY_ = 0;
};

// ---- Ui ---------------------------------------------------------------------------------------------------------

void Ui::show(PageId id, bool animate) {
    if (id == PageId::Welcome || id == PageId::Maintenance) {
        // Back from an error or the uninstall page: look at the PC again (the running copies, the installed version).
        if (!m.demo) m.existing = detect();
        m.action = actionFor(m.existing, m.info.version);
        if (m.existing.installed) m.dir = m.existing.dir;
        id = m.existing.installed ? PageId::Maintenance : PageId::Welcome;
    }
    std::unique_ptr<Page> next;
    switch (id) {
    case PageId::Welcome:
    case PageId::Maintenance: next = std::make_unique<WelcomePage>(*this); break;
    case PageId::Uninstall: next = std::make_unique<UninstallPage>(*this); break;
    case PageId::Progress: next = std::make_unique<ProgressPage>(*this); break;
    case PageId::Done: next = std::make_unique<DonePage>(*this, false); break;
    case PageId::Error: next = std::make_unique<DonePage>(*this, true); break;
    }
    // Forward to the progress / done pages and into the uninstall page; back otherwise.
    const float dir = id == PageId::Welcome || id == PageId::Maintenance ? -1.f : 1.f;
    const bool anim = animate && page_ != nullptr;
    if (page_) {
        page_->leave();
        if (!anim) root_->removeChild(page_);
    }
    // Leaving pages that finished their fade go now.
    std::vector<ui::Widget*> gone;
    for (auto& ch : root_->children())
        if (auto* p = dynamic_cast<Page*>(ch.get()); p && p != page_ && p->gone()) gone.push_back(p);
    for (auto* g : gone) root_->removeChild(g);

    next->enter(anim, dir);
    page_ = static_cast<Page*>(root_->adopt(std::move(next)));
    pageId_ = id;
    root_->close_->setEnabled(id != PageId::Progress);
    root_->requestLayout();
    window.setFocus(page_->initialFocus());
    window.invalidate();
    if (id == PageId::Progress) {
        finished_ = false;
        progressSince_ = ui::frame::realNow();
        root_->failed = false;
        root_->badge.snap(0);
        root_->shown.snap(0);
        root_->spinSpeed.to(m.job == Action::Uninstall ? -0.35f : 0.45f, ms(600), ui::Ease::Standard);
    } else if (id == PageId::Done || id == PageId::Error) {
        root_->failed = id == PageId::Error;
        root_->spinSpeed.to(0, ms(1100), ui::Ease::Decelerate);
        root_->badge.snap(0);
        root_->badge.to(1, ms(motion::slow), ui::Ease::Decelerate);
        if (id == PageId::Done && m.job != Action::Uninstall) root_->shown.to(1, ms(motion::normal));
        if (id == PageId::Error) root_->shown.to(0, ms(motion::slow));
        if (id == PageId::Done && m.job == Action::Uninstall) root_->shown.to(0, ms(motion::slow));
    } else {
        root_->failed = false;
        root_->badge.to(0, ms(motion::fast));
        root_->spinSpeed.to(0, ms(motion::slow));
        root_->shown.to(0, ms(motion::slow));
    }
}

void Ui::startJob(Action job) {
    if (busy() || m.demo) return;
    m.job = job;
    m.existing = detect();   // what runs now, for the steps
    m.jobHadRunning = !m.existing.running.empty();
    m.result = {};
    step_ = job == Action::Uninstall ? Step::Close : Step::Verify;
    show(PageId::Progress);
    Plan plan;
    plan.action = job;
    plan.dir = m.dir;
    plan.startMenu = m.startMenu;
    plan.desktop = m.desktop;
    plan.autostart = m.autostart;
    plan.removeData = !m.keepData;
    if (job == Action::Uninstall)
        ST_LOG_INFO("setup", "uninstall {} ({})", toUtf8(m.existing.dir.wstring()), m.keepData ? "keeping the user data" : "with the user data");
    else
        ST_LOG_INFO("setup", "install into {} (start menu {}, desktop {}, autostart {}, launch {})", toUtf8(plan.dir.wstring()), m.startMenu,
                    m.desktop, m.autostart, m.launchWhenDone);
    taskbar(0);
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread([this, plan, payload = m.payload] {
        Result r = run(plan, payload, [this](Step s, float f) { Dispatcher::post([this, s, f] { onProgress(s, f); }); });
        Dispatcher::post([this, r = std::move(r)]() mutable { setFinished(std::move(r)); });
    });
}

void Ui::onProgress(Step s, float overall) {
    if (pageId_ != PageId::Progress) return;
    step_ = s;
    root_->shown.to(std::max(overall, root_->shown.target()), ms(320), ui::Ease::Standard);
    taskbar(overall);
    window.invalidate();
}

void Ui::update() {
    const double now = ui::frame::realNow();
    // Pages that finished fading out go.
    std::vector<ui::Widget*> gone;
    for (auto& ch : root_->children())
        if (auto* p = dynamic_cast<Page*>(ch.get()); p && p != page_ && p->gone()) gone.push_back(p);
    for (auto* g : gone) root_->removeChild(g);
    if (pageId_ == PageId::Progress && finished_) {
        if (!m.result.ok) {
            taskbar(1, true);
            exitCode = kExitFailed;
            show(PageId::Error);
            return;
        }
        step_ = Step::Done;
        root_->shown.to(1, ms(320), ui::Ease::Standard);
        // On screen long enough to read (1.2 s; 0.4 s without motion) and the ticks caught up.
        if (!root_->shown.running() && now - progressSince_ >= (calm() ? 400.0 : 1200.0)) {
            exitCode = kExitOk;
            taskbar(-1);
            show(PageId::Done);
            if (m.job != Action::Uninstall && m.launchWhenDone && !m.result.exe.empty()) {
                installer::launch(m.result.exe, m.job == Action::Update ? L"--updated" : L"--installed");
                autoCloseAt_ = now + 1800;
            }
        }
        window.invalidate();
    }
    if (autoCloseAt_ > 0 && now >= autoCloseAt_) {
        autoCloseAt_ = 0;
        requestClose();
    }
}

double Ui::nextWake() const {
    double wake = 1e300;
    if (pageId_ == PageId::Progress && finished_) wake = ui::frame::realNow() + 30;
    if (autoCloseAt_ > 0) wake = std::min(wake, autoCloseAt_);
    return wake;
}

void Ui::pickFolder() {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(tr(L"Kurulum klasörünü seç"));
    // Starts where the current choice is (its parent when "ShadeTube" does not exist yet).
    fs::path start = m.dir;
    std::error_code ec;
    while (!start.empty() && !fs::exists(start, ec) && start != start.parent_path()) start = start.parent_path();
    ComPtr<IShellItem> folder;
    if (!start.empty() && SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr, IID_PPV_ARGS(&folder)))) d->SetFolder(folder.Get());
    if (FAILED(d->Show(window.hwnd()))) return;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(d->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
    m.dir = folderFor(path);
    CoTaskMemFree(path);
    m.dirOk = writable(m.dir);
    m.freeBytes = freeSpace(m.dir);
    ST_LOG_INFO("setup", "folder {} ({})", toUtf8(m.dir.wstring()), m.dirOk ? "writable" : "not writable");
    if (auto* w = dynamic_cast<WelcomePage*>(page_)) w->refresh();
}

void Ui::taskbar(float overall, bool error) {
    if (m.demo) return;
    if (!taskbar_ && FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar_)))) return;
    if (overall < 0) {
        taskbar_->SetProgressState(window.hwnd(), TBPF_NOPROGRESS);
        return;
    }
    taskbar_->SetProgressState(window.hwnd(), error ? TBPF_ERROR : TBPF_NORMAL);
    taskbar_->SetProgressValue(window.hwnd(), static_cast<ULONGLONG>(std::clamp(overall, 0.f, 1.f) * 1000), 1000);
}

// ---- Window setup ---------------------------------------------------------------------------------------------

bool systemLight() {
    DWORD value = 1, size = sizeof value;
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme",
                        RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS ||
           value != 0;
}

// The look the app itself has: its theme mode from the profile's settings.json (dark when there is none, the app's
// default), "Sistem" following Windows. Plus "Hareketi azalt".
struct Look {
    bool light = false, followSystem = false, reduceMotion = false;
};
Look appLook(const Args& args) {
    Look l;
    std::string theme = "dark";
    try {
        std::ifstream f(paths::settingsFile(), std::ios::binary);
        if (f) {
            const auto j = nlohmann::json::parse(f, nullptr, false);
            if (j.is_object()) {
                if (auto t = j.find("theme"); t != j.end() && t->is_string()) theme = t->get<std::string>();
                if (auto r = j.find("reduceMotion"); r != j.end() && r->is_boolean()) l.reduceMotion = r->get<bool>();
            }
        }
    } catch (const std::exception&) {
    }
    l.followSystem = theme == "system";
    l.light = theme == "light" || (l.followSystem && systemLight());
    if (args.light) {
        l.light = *args.light;
        l.followSystem = false;
    }
    return l;
}

std::unique_ptr<ui::Window> makeWindow(const Args& args, bool hidden) {
    ui::Window::setClassName(L"ShadeTube.Setup");   // never the app's class: running-instance lookups must not see us
    ui::WindowOptions o;
    o.title = tr(L"ShadeTube kurulumu");
    o.width = static_cast<int>(kW);
    o.height = static_cast<int>(kH);
    o.minWidth = static_cast<int>(kW);
    o.minHeight = static_cast<int>(kH);
    o.resizable = false;
    auto w = std::make_unique<ui::Window>(o);
    // The layout is fixed at kW × kH DIPs. --scale draws it at that scale whatever the monitor (screenshots); a work
    // area too small for it at the monitor's scale (1366 × 768 at 125 %) gets it zoomed out until it fits.
    float scale = args.scale > 0 ? args.scale : w->dpiScale();
    if (args.scale <= 0) {
        MONITORINFO mi{sizeof mi};
        if (GetMonitorInfoW(MonitorFromWindow(w->hwnd(), MONITOR_DEFAULTTOPRIMARY), &mi)) {
            const float fitW = (mi.rcWork.right - mi.rcWork.left) * 0.96f / kW, fitH = (mi.rcWork.bottom - mi.rcWork.top) * 0.96f / kH;
            scale = std::max(0.5f, std::min({scale, fitW, fitH}));
        }
    }
    if (std::abs(scale - w->dpiScale()) > 0.001f) {
        w->setZoom(scale / w->dpiScale());
        const int pw = static_cast<int>(std::lround(kW * scale)), ph = static_cast<int>(std::lround(kH * scale));
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(w->hwnd(), MONITOR_DEFAULTTOPRIMARY), &mi);
        const RECT& wa = mi.rcWork;
        SetWindowPos(w->hwnd(), nullptr, wa.left + (wa.right - wa.left - pw) / 2, wa.top + (wa.bottom - wa.top - ph) / 2, pw, ph,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    // Windows 11 rounds a window without a sizing frame only when asked.
    const DWORD round = 2;   // DWMWCP_ROUND
    DwmSetWindowAttribute(w->hwnd(), 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof round);
    (void)hidden;
    return w;
}

void initModel(Model& m, const Args& args, std::string_view payload, const payload::Info& info) {
    m.args = args;
    m.payload = payload;
    m.info = info;
    m.existing = detect();
    m.action = actionFor(m.existing, info.version);
    m.dir = m.existing.installed ? m.existing.dir : (args.dir.empty() ? defaultFolder() : fs::absolute(fs::path(args.dir)));
    m.dirOk = writable(m.dir);
    m.freeBytes = freeSpace(m.dir);
    m.startMenu = m.existing.installed ? m.existing.startMenu : !args.noStartMenu;
    m.desktop = m.existing.installed ? m.existing.desktop : true;
    if (args.desktop) m.desktop = true;
    m.autostart = m.existing.autostart || args.autostart;
}

} // namespace

int runWindow(const Args& args, std::string_view payload, const payload::Info& info, const std::wstring& payloadError) {
    const Look look = appLook(args);
    motion::setReduced(look.reduceMotion);
    gfx::Theme::get().load(look.light);
    Dispatcher::init();
    Model m;
    if (payloadError.empty()) initModel(m, args, payload, info);
    m.payloadError = payloadError;
    auto window = makeWindow(args, false);
    Ui ui(m, *window);
    window->onDestroyed = [] { PostQuitMessage(0); };
    window->onMessage = [&](UINT msg, WPARAM, LPARAM lp) {
        // "Sistem": Windows switched between light and dark.
        if (msg == WM_SETTINGCHANGE && look.followSystem && lp && std::wstring_view(reinterpret_cast<const wchar_t*>(lp)) == L"ImmersiveColorSet") {
            gfx::Theme::get().load(systemLight());
            window->applyTheme();
        }
    };
    if (!payloadError.empty()) {
        m.job = Action::Install;
        ui.show(PageId::Error, false);
        ui.exitCode = kExitFailed;
    } else {
        ui.show(m.existing.installed ? PageId::Maintenance : PageId::Welcome, false);
    }
    window->show(SW_SHOW);
    SetForegroundWindow(window->hwnd());

    MSG msg;
    bool running = true;
    while (running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        ui.update();
        if (!IsWindow(window->hwnd())) break;
        if (window->needsFrame()) {
            if (HANDLE h = window->frameWaitable()) WaitForSingleObjectEx(h, 50, TRUE);
            window->render();
            continue;
        }
        const double wake = std::min(window->nextWakeMs(), ui.nextWake());
        const DWORD timeout = static_cast<DWORD>(std::clamp(wake - ui::frame::realNow(), 0.0, 60000.0));
        MsgWaitForMultipleObjectsEx(0, nullptr, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    const int code = ui.exitCode;
    ST_LOG_INFO("setup", "window closed (exit {})", code);
    Dispatcher::shutdown();
    return code;
}

int screenshots(const Args& args, std::string_view payload, const payload::Info& info) {
    const Look look = appLook(args);
    gfx::Theme::get().load(look.light);
    Dispatcher::init();
    const fs::path dir = args.screenshotDir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    Model m;
    m.demo = true;
    m.args = args;
    m.payload = payload;
    m.info = info;
    m.existing = {};
    m.existing.dir = defaultFolder();
    m.dir = m.existing.dir;
    m.freeBytes = 118ll << 30;
    m.desktop = true;
    auto window = makeWindow(args, true);
    Ui ui(m, *window);
    int failures = 0;
    const std::wstring older = app::updater::compareVersions(info.version, "0.10.0") > 0 ? L"0.10.0" : L"0.0.1";
    auto shot = [&](const char* name, const std::function<void()>& setUp) {
        if (!args.page.empty() && args.page != name) return;
        setUp();
        window->invalidate();
        const fs::path png = dir / (toWide(name) + L".png");
        const bool ok = window->renderToPng(png.wstring());
        std::printf("%s %ls\n", ok ? "ok  " : "FAIL", png.c_str());
        if (!ok) ++failures;
    };
    auto installed = [&](const std::string& version, bool running) {
        m.existing = {};
        m.existing.installed = true;
        m.existing.dir = defaultFolder();
        m.existing.exe = m.existing.dir / L"ShadeTube.exe";
        m.existing.version = version;
        m.existing.startMenu = true;
        if (running) m.existing.running.push_back({1, 1, m.existing.exe});
    };
    auto fresh = [&](bool running) {
        m.existing = {};
        m.existing.dir = defaultFolder();
        if (running) m.existing.running.push_back({1, 1, L"C:\\ShadeTube\\ShadeTube.exe"});
        m.dirOk = true;
    };
    auto progress = [&](Action job, Step step, float value, bool hadRunning) {
        m.job = job;
        m.jobHadRunning = hadRunning;
        ui.setStep(step);
        ui.show(PageId::Progress, false);
        ui.root().shown.snap(value);
        ui.root().spinSpeed.snap(0);
        ui.root().angle = 28;
    };
    auto finish = [&](Action job, bool ok, PageId page) {
        m.job = job;
        m.result = {};
        m.result.ok = ok;
        m.result.exe = m.dir / L"ShadeTube.exe";
        ui.show(page, false);
        ui.root().badge.snap(1);
        ui.root().shown.snap(job == Action::Uninstall || !ok ? 0.f : 1.f);
        ui.root().spinSpeed.snap(0);
        ui.root().angle = 0;
    };
    shot("welcome", [&] {
        fresh(false);
        ui.show(PageId::Welcome, false);
    });
    shot("welcome-running", [&] {
        fresh(true);
        ui.show(PageId::Welcome, false);
    });
    shot("folder-error", [&] {
        fresh(false);
        m.dir = L"C:\\Program Files\\ShadeTube";
        m.dirOk = false;
        ui.show(PageId::Welcome, false);
    });
    m.dir = defaultFolder();
    m.dirOk = true;
    shot("update", [&] {
        installed(toUtf8(older), true);
        ui.show(PageId::Maintenance, false);
    });
    shot("repair", [&] {
        installed(info.version, false);
        ui.show(PageId::Maintenance, false);
    });
    shot("downgrade", [&] {
        installed("9.0.0", false);
        ui.show(PageId::Maintenance, false);
    });
    shot("uninstall", [&] {
        installed(info.version, true);
        ui.show(PageId::Uninstall, false);
    });
    shot("progress", [&] {
        fresh(false);
        progress(Action::Install, Step::Unpack, 0.62f, true);
    });
    shot("progress-update", [&] {
        installed(toUtf8(older), false);
        progress(Action::Update, Step::Register, 0.93f, false);
    });
    shot("progress-uninstall", [&] {
        installed(info.version, false);
        progress(Action::Uninstall, Step::Remove, 0.4f, true);
    });
    shot("done", [&] {
        fresh(false);
        m.launchWhenDone = false;
        finish(Action::Install, true, PageId::Done);
    });
    shot("done-launch", [&] {
        fresh(false);
        m.launchWhenDone = true;
        finish(Action::Install, true, PageId::Done);
    });
    shot("done-update", [&] {
        installed(toUtf8(older), false);
        m.launchWhenDone = false;
        finish(Action::Update, true, PageId::Done);
        m.result.stillRunning = true;
    });
    shot("uninstalled", [&] {
        installed(info.version, false);
        m.keepData = true;
        finish(Action::Uninstall, true, PageId::Done);
    });
    shot("error", [&] {
        fresh(false);
        finish(Action::Install, false, PageId::Error);
        m.result.error = i18n::format(tr(L"Kurulum klasörü oluşturulamadı ({})."), {L"D:\\Music\\ShadeTube"});
        ui.show(PageId::Error, false);
        ui.root().badge.snap(1);
    });
    window.reset();
    Dispatcher::shutdown();
    return failures;
}

} // namespace st::setup
