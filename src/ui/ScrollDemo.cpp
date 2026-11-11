// Dev check of smooth scrolling on the real window (docs/DEVELOPMENT.md 5.4): SHADETUBE_SCROLL_DEMO=<steps>[@x,y]
// posts wheel / key messages to the window from a timer thread, at the rhythm real devices send them, while every
// frame of each moving ScrollView is recorded (in memory: no file I/O in the frame loop) and logged after the script
// ("scroll" lines: frame gap, offset, step, velocity, target, feel, CPU time of the frame up to the view's paint). A
// summary per view follows: frames, frame gaps over 1.5x the median (dropped frames), the longest gap, direction
// changes of the motion (should match the reversals in the script) and the largest change of speed between two frames
// (DIP/ms: smoothness). Gaps between two motions (the view at rest) are not counted.
//
// Steps (comma separated, run in order with 700 ms between them; default notch,spin,touch,keys):
//   notch   6 notches down, 140 ms apart (one glide each)
//   spin    24 notches down 20 ms apart (acceleration), then 3 up (reversal)
//   touch   a precision-touchpad swipe down (small deltas every 8 ms + Windows-style inertia), then one up
//   keys    PageDown, PageDown, End, Home, Down x 8 (key repeat)
//   end     80 fast notches down (into the end), then 80 up (into the top)
#include "core/Log.h"
#include "core/Utf.h"
#include "ui/Layout.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace st::ui {

namespace {

bool g_trace = false;

struct ViewStats {
    int frames = 0;
    bool running = false;   // the last recorded frame was in motion (its gap to this one counts)
    double last = 0, lastSpeed = 0;
    float lastY = 0;
    int lastDir = 0, dirChanges = 0;
    double maxSpeedChange = 0, maxCpu = 0;
    std::vector<double> gaps;
};
std::map<const ScrollView*, ViewStats> g_stats;

struct Record {
    const ScrollView* view;
    double gap, y, step, velocity, target, cpu;
    ScrollFeel feel;
};
std::vector<Record> g_records;

struct Event {
    double at;   // ms after the start
    UINT msg;
    WPARAM wp;
    LPARAM lp;
};

const char* feelName(ScrollFeel f) {
    switch (f) {
    case ScrollFeel::Wheel: return "wheel";
    case ScrollFeel::Precision: return "precise";
    case ScrollFeel::Glide: return "glide";
    default: return "follow";
    }
}

void logSummary() {
    for (const Record& r : g_records)
        ST_LOG_INFO("scroll", "{} gap {:5.1f} y {:8.2f} step {:+7.2f} v {:+7.0f} -> {:8.1f} {} cpu {:.2f}",
                    static_cast<const void*>(r.view), r.gap, r.y, r.step, r.velocity, r.target, feelName(r.feel),
                    r.cpu);
    for (auto& [view, s] : g_stats) {
        if (s.frames < 2) continue;
        std::vector<double> g = s.gaps;
        std::sort(g.begin(), g.end());
        const double median = g.empty() ? 0 : g[g.size() / 2];
        const auto slow = std::count_if(s.gaps.begin(), s.gaps.end(), [&](double x) { return x > median * 1.5; });
        ST_LOG_INFO("scroll",
                    "summary {}: {} frames, median gap {:.2f} ms, {} over 1.5x, longest {:.1f} ms, max frame cpu "
                    "{:.2f} ms, {} direction changes, max speed change {:.3f} DIP/ms between frames",
                    static_cast<const void*>(view), s.frames, median, slow, g.empty() ? 0.0 : g.back(), s.maxCpu,
                    s.dirChanges, s.maxSpeedChange);
    }
    g_stats.clear();
    g_records.clear();
    g_trace = false;
    ST_LOG_INFO("scroll", "demo: done");
}

// Posts the events at their times (a high-resolution waitable timer: 1 ms accuracy, as a device would deliver).
void play(HWND hwnd, std::vector<Event> events) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER freq, start;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (const Event& e : events) {
        for (;;) {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const double elapsed =
                static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
            const double wait = e.at - elapsed;
            if (wait <= 0.05) break;
            if (timer) {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>(wait * 10000.0);   // relative, 100 ns units
                SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(static_cast<DWORD>(std::max(1.0, wait)));
            }
        }
        PostMessageW(hwnd, e.msg, e.wp, e.lp);
    }
    if (timer) CloseHandle(timer);
}

} // namespace

bool scrollTraceOn() { return g_trace; }

void traceScrollFrame(const ScrollView* view, float y, float target, float velocity, ScrollFeel feel, bool moving) {
    ViewStats& s = g_stats[view];
    const double now = frame::now(), cpu = frame::realNow() - frame::begunAt();
    const bool counted = s.running && now > s.last;
    const double gap = counted ? now - s.last : 0;
    const double step = s.frames ? y - s.lastY : 0;
    if (counted) {
        s.gaps.push_back(gap);
        const int dir = step > 0.01 ? 1 : step < -0.01 ? -1 : 0;
        if (dir && s.lastDir && dir != s.lastDir) ++s.dirChanges;
        if (dir) s.lastDir = dir;
        const double speed = step / gap;
        if (s.gaps.size() > 1) s.maxSpeedChange = std::max(s.maxSpeedChange, std::abs(speed - s.lastSpeed));
        s.lastSpeed = speed;
    } else {
        s.lastSpeed = 0;
    }
    s.maxCpu = std::max(s.maxCpu, cpu);
    ++s.frames;
    s.running = moving;
    s.last = now;
    s.lastY = y;
    g_records.push_back({view, gap, y, step, velocity, target, cpu, feel});
}

void startScrollDemo(Window& w) {
    wchar_t spec[256] = {};
    if (!GetEnvironmentVariableW(L"SHADETUBE_SCROLL_DEMO", spec, 256)) return;
    static std::wstring s_spec;
    static Window* s_window = nullptr;
    s_spec = spec;
    s_window = &w;
    g_trace = true;
    g_records.reserve(20000);
    SetTimer(nullptr, 0, 2500, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        Window& win = *s_window;
        std::wstring steps = s_spec;
        const Rect client = win.clientRect();
        Point at{client.w * 0.6f, client.h * 0.5f};
        if (const size_t pos = steps.find(L'@'); pos != std::wstring::npos) {
            at.x = static_cast<float>(_wtof(steps.c_str() + pos + 1));
            if (const size_t comma = steps.find(L',', pos); comma != std::wstring::npos)
                at.y = static_cast<float>(_wtof(steps.c_str() + comma + 1));
            steps.resize(pos);
        }
        if (steps.empty() || steps == L"1" || steps == L"all") steps = L"notch,spin,touch,keys";
        const float scale = win.scale();
        POINT client_pt{static_cast<LONG>(at.x * scale), static_cast<LONG>(at.y * scale)};
        POINT screen = client_pt;
        ClientToScreen(win.hwnd(), &screen);
        const LPARAM screenLp = MAKELPARAM(screen.x, screen.y);

        std::vector<Event> ev;
        double t = 0;
        auto wheel = [&](int delta, double gap) {
            t += gap;
            ev.push_back({t, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(static_cast<SHORT>(delta))), screenLp});
        };
        auto key = [&](UINT vk, double gap, bool repeat = false) {
            t += gap;
            ev.push_back({t, WM_KEYDOWN, vk, static_cast<LPARAM>(1 | (repeat ? (1u << 30) : 0u))});
            ev.push_back({t + 1, WM_KEYUP, vk, static_cast<LPARAM>(1u | (1u << 30) | (1u << 31))});
        };
        size_t start = 0;
        while (start <= steps.size()) {
            const size_t comma = std::min(steps.find(L',', start), steps.size());
            const std::wstring step = steps.substr(start, comma - start);
            start = comma + 1;
            t += 700;
            if (step == L"notch") {
                for (int i = 0; i < 6; ++i) wheel(-WHEEL_DELTA, i ? 140 : 0);
            } else if (step == L"spin") {
                for (int i = 0; i < 24; ++i) wheel(-WHEEL_DELTA, i ? 20 : 0);
                t += 600;
                for (int i = 0; i < 3; ++i) wheel(WHEEL_DELTA, i ? 40 : 0);
            } else if (step == L"touch") {
                // Finger down: deltas of ~18 every 8 ms, uneven like a real pad; then Windows' inertia decays.
                for (int i = 0; i < 30; ++i) wheel(-(14 + (i * 7) % 9), i ? 8 : 0);
                double d = 18;
                for (int i = 0; i < 45 && d >= 1; ++i, d *= 0.92) wheel(-static_cast<int>(std::lround(d)), 10);
                t += 500;
                for (int i = 0; i < 20; ++i) wheel(16 + (i * 5) % 7, 8);
            } else if (step == L"keys") {
                key(VK_NEXT, 0);
                key(VK_NEXT, 500);
                key(VK_END, 600);
                key(VK_HOME, 800);
                for (int i = 0; i < 8; ++i) key(VK_DOWN, i ? 33 : 900, i > 0);
            } else if (step == L"end") {
                for (int i = 0; i < 80; ++i) wheel(-WHEEL_DELTA, i ? 15 : 0);
                t += 1200;
                for (int i = 0; i < 80; ++i) wheel(WHEEL_DELTA, i ? 15 : 0);
            } else if (!step.empty()) {
                ST_LOG_WARN("scroll", "demo: unknown step \"{}\"", toUtf8(step));
            }
        }
        ST_LOG_INFO("scroll", "demo: {} events over {:.0f} ms at {},{}", ev.size(), t, at.x, at.y);
        std::thread(play, win.hwnd(), std::move(ev)).detach();
        SetTimer(nullptr, 0, static_cast<UINT>(t + 1500), [](HWND, UINT, UINT_PTR id2, DWORD) {
            KillTimer(nullptr, id2);
            logSummary();
        });
    });
}

} // namespace st::ui
