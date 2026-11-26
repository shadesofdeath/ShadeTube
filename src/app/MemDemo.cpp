// SHADETUBE_MEM_DEMO (dev): see MemDemo.h.
#include "app/MemDemo.h"

#include "app/AppContext.h"
#include "app/MemProfile.h"
#include "app/Router.h"
#include "core/Log.h"
#include "core/MemoryStats.h"
#include "core/Paths.h"
#include "core/Utf.h"
#include "gfx/Device.h"
#include "gfx/Icons.h"
#include "gfx/ImageCache.h"
#include "ui/Acrylic.h"
#include "ui/Anim.h"
#include "ui/Window.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace st::app::memdemo {

namespace {

struct Step {
    std::string action, arg;
};
std::vector<Step> g_steps;
std::unordered_map<UINT_PTR, size_t> g_timers;
Hooks g_hooks;
double g_startedAt = 0;
bool g_active = false;
std::vector<double> g_frames;        // main-window frame CPU times since the last report
std::string g_afterStep;             // a trim step ran: the next frame's time is logged with its name
std::vector<std::string> g_afterLog;

std::string frameStats() {
    std::string out;
    if (!g_frames.empty()) {
        std::vector<double> f = g_frames;
        std::sort(f.begin(), f.end());
        double sum = 0;
        for (double v : f) sum += v;
        out += std::format("frames: {} since the last report, mean {:.2f} ms, p95 {:.2f} ms, slowest {:.2f} ms", f.size(),
                           sum / static_cast<double>(f.size()), f[std::min(f.size() - 1, f.size() * 95 / 100)], f.back());
    } else {
        out += "frames: none since the last report";
    }
    for (const auto& s : g_afterLog) out += "; " + s;
    out += "\n";
    g_frames.clear();
    g_afterLog.clear();
    return out;
}

double mb(uint64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

Route routeFrom(const std::string& r) {
    auto after = [&r](const char* prefix) -> std::optional<std::string> {
        const std::string p = prefix;
        if (r.rfind(p, 0) == 0) return r.substr(p.size());
        return std::nullopt;
    };
    if (r == "search") return {RouteKind::Search};
    if (auto v = after("search:")) return {RouteKind::Search, *v};
    if (auto v = after("artist:")) return {RouteKind::Artist, *v};
    if (auto v = after("album:")) return {RouteKind::Album, *v};
    if (auto v = after("playlist:")) return {RouteKind::Playlist, *v};
    if (r == "liked") return {RouteKind::Liked};
    if (r == "library") return {RouteKind::Library};
    if (auto v = after("library:")) return {RouteKind::Library, *v};
    if (r == "downloads") return {RouteKind::Downloads};
    if (r == "stats") return {RouteKind::Stats};
    if (r == "local") return {RouteKind::LocalFiles};
    if (auto v = after("local:")) return {RouteKind::LocalFiles, *v};
    if (r == "radio") return {RouteKind::Radio};
    if (r == "podcasts") return {RouteKind::Podcasts};
    if (r == "releases") return {RouteKind::NewReleases};
    if (auto v = after("smart:")) return {RouteKind::SmartList, *v};
    if (r == "settings") return {RouteKind::Settings};
    if (auto v = after("settings:")) return {RouteKind::Settings, *v};
    return {RouteKind::Home};
}

int countWidgets(const ui::Widget* w) {
    if (!w) return 0;
    int n = 1;
    for (const auto& c : w->children()) n += countWidgets(c.get());
    return n;
}

void report(const std::string& label) {
    const auto snap = mem::take();
    std::string out = std::format("=== {} at {:.1f} s ===\n", label.empty() ? "report" : label,
                                  (ui::frame::realNow() - g_startedAt) / 1000.0);
    out += mem::detailedReport();
    auto& dev = gfx::Device::get();
    const auto gpu = dev.gpuMemory();
    out += std::format("gpu: device {} (generation {}), local {:.1f} MB, non-local {:.1f} MB\n",
                       dev.ready() ? "ready" : "released", dev.generation(), mb(gpu.local), mb(gpu.nonLocal));
    const auto im = gfx::ImageCache::get().stats();
    out += std::format("images: {} bitmaps {:.1f} MB (budget {:.0f} MB), {} pending, {} failed, {} accents; by bucket",
                       im.ready, mb(im.bytes), mb(im.budget), im.pending, im.failed, im.accents);
    constexpr size_t kN = std::size(gfx::ImageCache::kBuckets);
    for (size_t b = 0; b <= kN; ++b)
        if (im.bucketCount[b])
            out += std::format(" {}px {}/{:.1f} MB", b < kN ? gfx::ImageCache::kBuckets[b] : 1280, im.bucketCount[b],
                               mb(im.bucketBytes[b]));
    out += "\n";
    out += std::format("icons: {} cached, {:.0f} KB; acrylic {:.0f} KB; audio buffers {:.1f} MB\n", gfx::Icons::count(),
                       gfx::Icons::memoryBytes() / 1024.0, ui::Acrylic::liveBytes() / 1024.0,
                       mb(ctx().player ? ctx().player->bufferedBytes() : 0));
    ui::Window* mini = g_hooks.miniWindow ? g_hooks.miniWindow() : nullptr;
    out += std::format("widgets: main window tree {}, mini player tree {}; library: {} liked, {} history, {} playlists; "
                       "queue {}\n",
                       countWidgets(ctx().window ? ctx().window->root() : nullptr), countWidgets(mini ? mini->root() : nullptr),
                       ctx().library.liked().size(), ctx().library.history().size(), ctx().library.playlists().size(),
                       ctx().player ? ctx().player->order().size() : 0);
    out += frameStats();
    out += memprofile::report();
    out += "\n";
    std::error_code ec;
    std::filesystem::create_directories(paths::logDir(), ec);
    std::ofstream(paths::logDir() / L"memreport.txt", std::ios::app) << out;
    ST_LOG_INFO("memdemo", "report \"{}\": {} | gpu {:.1f} + {:.1f} MB, images {:.1f} MB", label, mem::describe(snap),
                mb(gpu.local), mb(gpu.nonLocal), mb(im.bytes));
}

void wheel(int notches) {
    auto* w = ctx().window;
    if (!w) return;
    RECT rc{};
    GetClientRect(w->hwnd(), &rc);
    POINT pt{static_cast<LONG>(rc.right * 0.6), rc.bottom / 2};
    ClientToScreen(w->hwnd(), &pt);
    // Posted from a short-lived thread at a wheel's rhythm (the window's message loop keeps rendering meanwhile).
    const HWND h = w->hwnd();
    const int n = notches < 0 ? -notches : notches;
    const short delta = static_cast<short>(notches < 0 ? WHEEL_DELTA : -WHEEL_DELTA);
    const HANDLE thread = CreateThread(
        nullptr, 64 * 1024,
        [](void* p) -> DWORD {
            const auto* a = static_cast<std::tuple<HWND, POINT, int, short>*>(p);
            const auto [hwnd, at, count, d] = *a;
            delete a;
            for (int i = 0; i < count; ++i) {
                PostMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, d), MAKELPARAM(at.x, at.y));
                Sleep(60);
            }
            return 0;
        },
        new std::tuple<HWND, POINT, int, short>(h, pt, n, delta), STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (thread) CloseHandle(thread);
}

void run(const Step& s) {
    ST_LOG_INFO("memdemo", "{} {}", s.action, s.arg);
    auto& c = ctx();
    auto call = [](const std::function<void()>& f) {
        if (f) f();
    };
    if (s.action == "route") {
        if (c.router) c.router->navigate(routeFrom(s.arg));
    } else if (s.action == "back") {
        if (c.router) c.router->back();
    } else if (s.action == "forward") {
        if (c.router) c.router->forward();
    } else if (s.action == "wheel") {
        wheel(std::atoi(s.arg.c_str()));
    } else if (s.action == "np") {
        if (c.toggleNowPlaying) c.toggleNowPlaying(true);
    } else if (s.action == "npclose") {
        if (c.toggleNowPlaying) c.toggleNowPlaying(false);
    } else if (s.action == "next") {
        if (c.player) c.player->next();
    } else if (s.action == "tray") {
        call(g_hooks.tray);
    } else if (s.action == "show") {
        call(g_hooks.show);
    } else if (s.action == "minimize") {
        if (c.window) c.window->minimize();
    } else if (s.action == "restore") {
        if (c.window) ShowWindow(c.window->hwnd(), SW_RESTORE);
    } else if (s.action == "mini") {
        call(g_hooks.mini);
    } else if (s.action == "miniclose") {
        call(g_hooks.miniClose);
    } else if (s.action == "trim") {
        call(g_hooks.trim);
        g_afterStep = "trim";
    } else if (s.action == "gputrim") {
        gfx::Device::get().trimPool();
        g_afterStep = "gputrim";
    } else if (s.action == "gpuclear") {
        if (auto* d = gfx::Device::get().d2dDevice()) d->ClearResources(0);
        g_afterStep = "gpuclear";
    } else if (s.action == "d2dcache") {   // Direct2D's texture cache budget in MB (gfx::Device sets 24)
        if (auto* d = gfx::Device::get().d2dDevice()) d->SetMaximumTextureMemory(std::stoull(s.arg) << 20);
        g_afterStep = "d2dcache";
    } else if (s.action == "report") {
        report(s.arg);
    } else if (s.action == "frames") {   // only the frame times since the last report / frames step
        std::ofstream(paths::logDir() / L"memreport.txt", std::ios::app) << "--- " << s.arg << ": " << frameStats();
        return;
    } else if (s.action == "quit") {
        call(g_hooks.quit);
    } else {
        ST_LOG_WARN("memdemo", "unknown step \"{}\"", s.action);
        return;
    }
    if (c.window) c.window->invalidate();
}

void CALLBACK tick(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    const auto it = g_timers.find(id);
    if (it == g_timers.end()) return;
    const size_t index = it->second;
    g_timers.erase(it);
    if (index < g_steps.size()) run(g_steps[index]);
}

} // namespace

void noteFrame(double cpuMs) {
    if (!g_active) return;
    g_frames.push_back(cpuMs);
    if (!g_afterStep.empty()) {
        g_afterLog.push_back(std::format("first frame after {}: {:.2f} ms", g_afterStep, cpuMs));
        g_afterStep.clear();
    }
}

void schedule(Hooks hooks) {
    std::wstring spec(32768, L'\0');
    const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_MEM_DEMO", spec.data(), static_cast<DWORD>(spec.size()));
    if (n == 0 || n >= spec.size()) return;
    spec.resize(n);
    g_hooks = std::move(hooks);
    g_startedAt = ui::frame::realNow();
    g_active = true;
    std::string all = toUtf8(spec);
    std::error_code ec;
    if (std::filesystem::is_regular_file(std::filesystem::path(spec), ec)) {   // a file of steps
        std::ifstream f{std::filesystem::path(spec)};
        std::stringstream ss;
        ss << f.rdbuf();
        all = ss.str();
        for (char& ch : all)
            if (ch == '\n' || ch == '\r') ch = ';';
    }
    for (size_t start = 0; start < all.size();) {
        const size_t end = std::min(all.find(';', start), all.size());
        std::string step = all.substr(start, end - start);
        start = end + 1;
        while (!step.empty() && (step.front() == ' ' || step.front() == '\t')) step.erase(0, 1);
        const size_t at = step.find('@');
        if (at == std::string::npos || step.starts_with("#")) continue;
        const size_t eq = step.find('=', at);
        const int ms = std::atoi(step.substr(at + 1, eq == std::string::npos ? std::string::npos : eq - at - 1).c_str());
        g_steps.push_back({step.substr(0, at), eq == std::string::npos ? std::string() : step.substr(eq + 1)});
        const UINT_PTR id = SetTimer(nullptr, 0, static_cast<UINT>(std::max(1, ms)), tick);
        if (id) g_timers[id] = g_steps.size() - 1;
    }
    ST_LOG_INFO("memdemo", "{} steps scheduled", g_steps.size());
}

} // namespace st::app::memdemo
