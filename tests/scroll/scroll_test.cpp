// Developer test for smooth scrolling (ui/ScrollPhysics), stepped with explicit times (deterministic), offline:
//   notch      one notch glides monotonically to its step (no overshoot), ~95 % within 120..250 ms, then settles
//   retarget   a notch in flight keeps position and velocity (no restart, no jerk); finite-difference speed has no jump
//   accel      slow notches add up exactly; a fast run accelerates gently (first notches plain, capped at maxAccel)
//   reverse    reversing mid-glide starts from the current position and turns around within ~100 ms
//   clamp      spinning into the end never passes it (monotonic, lands exactly, velocity 0); an overshoot after a
//              change of feel stops hard at the bound; narrowing the bounds clamps without bouncing
//   precision  touchpad / high-resolution deltas: target moves 1:1 (no acceleration), position trails by < 1.3 frames,
//              catches up within 60 ms; a whole notch within the hold window still counts as precision, later not
//   rate       the same input at 60 Hz, 144 Hz and uneven frame times gives the same positions
//   keys       key repeat (glides retargeted every 33 ms) moves monotonically and lands on the sum
//   feels      Wheel < Glide < Follow in time to 95 %
//   instant    reduce motion: every move lands at once
//   frames     SmoothScroll (frame clock) asks for frames only while it moves; wheel::step follows the system setting
//   scroll_test
#include "ui/Anim.h"
#include "ui/ScrollPhysics.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <random>
#include <vector>

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

constexpr double kFrame60 = 1000.0 / 60.0;

struct Sample {
    double t;
    float x, v;
};

// Advances `p` frame by frame from `from` to `to` (inclusive of `to`), recording every frame.
std::vector<Sample> run(ScrollPhysics& p, double from, double to, double frameMs) {
    std::vector<Sample> out;
    for (double t = from; t < to + 1e-9; t += frameMs) {
        p.advance(t);
        out.push_back({t, p.position(), p.velocity()});
    }
    return out;
}

bool monotonic(const std::vector<Sample>& s, int dir) {
    for (size_t i = 1; i < s.size(); ++i)
        if ((s[i].x - s[i - 1].x) * dir < -1e-4f) return false;
    return true;
}

double timeTo(const std::vector<Sample>& s, float from, float to, double fraction) {
    for (const auto& x : s)
        if (std::abs(x.x - from) >= std::abs(to - from) * fraction) return x.t;
    return 1e9;
}

void testNotch() {
    std::printf("notch\n");
    ScrollPhysics p;
    p.setBounds(0, 10000, 0);
    CHECK(p.wheel(-1, 120, 0));   // one notch toward the end
    CHECK(p.target() == 120.f);
    CHECK(p.moving());
    const auto s = run(p, 0, 600, kFrame60);
    CHECK(monotonic(s, 1));
    float maxX = 0;
    for (const auto& x : s) maxX = std::max(maxX, x.x);
    CHECK(maxX <= 120.f + 1e-4f);
    const double t95 = timeTo(s, 0, 120, 0.95);
    std::printf("        95 %% after %.0f ms, first frame %.1f DIP\n", t95, s[1].x);
    CHECK(t95 >= 120 && t95 <= 250);
    CHECK(s[1].x > 3.f);   // starts moving on the first frame (no dead start)
    CHECK(!p.moving());
    CHECK(p.position() == 120.f && p.velocity() == 0.f);
    // Settled: later evaluations change nothing.
    p.advance(5000);
    CHECK(!p.moving() && p.position() == 120.f);
}

void testRetarget() {
    std::printf("retarget\n");
    ScrollPhysics p;
    p.setBounds(0, 10000, 0);
    p.wheel(-1, 120, 0);
    std::vector<Sample> s = run(p, 0, 50, kFrame60 / 4);
    p.advance(70);
    const float x0 = p.position(), v0 = p.velocity();
    p.wheel(-1, 120, 70);   // second notch while the first is in flight
    CHECK(p.position() == x0);
    CHECK(std::abs(p.velocity() - v0) < 1e-3f);
    CHECK(p.target() == 240.f);
    // Finite-difference speed across the retarget: it changes no more than when a notch starts from rest (the spring's
    // own acceleration), never by the in-flight speed (an eased restart from the current value jumps or stalls).
    auto maxSpeedChange = [](const std::vector<Sample>& v) {
        double m = 0;
        for (size_t i = 2; i < v.size(); ++i) {
            const double va = (v[i - 1].x - v[i - 2].x) / (v[i - 1].t - v[i - 2].t);
            const double vb = (v[i].x - v[i - 1].x) / (v[i].t - v[i - 1].t);
            m = std::max(m, std::abs(vb - va));
        }
        return m;
    };
    const auto after = run(p, 70 + kFrame60 / 4, 200, kFrame60 / 4);
    std::vector<Sample> all;   // around the retarget only (the first notch's own start is the baseline below)
    for (const auto& q : s)
        if (q.t >= 30) all.push_back(q);
    all.push_back({70, x0, v0});
    all.insert(all.end(), after.begin(), after.end());
    const double maxJump = maxSpeedChange(all);
    ScrollPhysics fresh;
    fresh.setBounds(0, 10000, 0);
    fresh.wheel(-1, 120, 0);
    std::vector<Sample> from0{{0, 0, 0}};
    const auto f = run(fresh, kFrame60 / 4, 200, kFrame60 / 4);
    from0.insert(from0.end(), f.begin(), f.end());
    const double baseline = maxSpeedChange(from0);
    std::printf("        speed %.0f DIP/s at the retarget; largest change between quarter frames %.3f DIP/ms (a notch "
                "from rest: %.3f)\n", v0, maxJump, baseline);
    CHECK(maxJump <= baseline * 1.05);
    CHECK(monotonic(after, 1));
    run(p, 200, 900, kFrame60);
    CHECK(!p.moving() && p.position() == 240.f);
}

void testAccel() {
    std::printf("accel\n");
    {
        ScrollPhysics p;
        p.setBounds(0, 100000, 0);
        for (int i = 0; i < 5; ++i) {
            p.wheel(-1, 120, i * 200.0);
            run(p, i * 200.0, i * 200.0 + 190, kFrame60);
        }
        CHECK(p.target() == 600.f);   // slow notches: exactly 5 steps
    }
    {
        ScrollPhysics p;
        p.setBounds(0, 100000, 0);
        const auto& tune = p.tuning();
        std::vector<float> steps;
        float last = 0;
        for (int i = 0; i < 40; ++i) {
            p.wheel(-1, 120, i * 20.0);
            steps.push_back(p.target() - last);
            last = p.target();
            run(p, i * 20.0, i * 20.0 + 19, kFrame60);
        }
        bool firstPlain = true, nonDecreasing = true, capped = true;
        for (int i = 0; i <= tune.freeNotches; ++i) firstPlain = firstPlain && std::abs(steps[i] - 120.f) < 1e-3f;
        for (size_t i = 1; i < steps.size(); ++i) nonDecreasing = nonDecreasing && steps[i] >= steps[i - 1] - 1e-3f;
        for (float st : steps) capped = capped && st <= 120.f * static_cast<float>(tune.maxAccel) + 1e-3f;
        std::printf("        40 notches 20 ms apart: %.0f DIP (plain 4800), steps %.0f .. %.0f\n", p.target(),
                    steps.front(), steps.back());
        CHECK(firstPlain);
        CHECK(nonDecreasing);
        CHECK(capped);
        CHECK(p.target() > 4800.f * 1.5f);
        CHECK(steps[10] < 120.f * 2.2f);   // gentle: after 10 fast notches not yet more than ~2x
        // The run breaks after a pause: plain steps again.
        run(p, 800, 1200, kFrame60);
        const float before = p.target();
        p.wheel(-1, 120, 1200);
        CHECK(std::abs(p.target() - before - 120.f) < 1e-3f);
    }
}

void testReverse() {
    std::printf("reverse\n");
    ScrollPhysics p;
    p.setBounds(0, 100000, 0);
    for (int i = 0; i < 12; ++i) {
        p.wheel(-1, 120, i * 20.0);
        run(p, i * 20.0, i * 20.0 + 19, kFrame60);
    }
    p.advance(240);
    const float x = p.position(), farTarget = p.target();
    CHECK(farTarget - x > 200.f);   // far target ahead of the content
    p.wheel(1, 120, 240);           // one notch back
    CHECK(std::abs(p.target() - (x - 120.f)) < 1e-3f);
    const auto s = run(p, 240, 340, kFrame60);
    std::printf("        reversed at %.0f (target was %.0f), speed after 100 ms %.0f DIP/s\n", x, farTarget,
                s.back().v);
    CHECK(s.back().v < 0);
    run(p, 340, 1000, kFrame60);
    CHECK(!p.moving() && std::abs(p.position() - (x - 120.f)) < 1e-3f);
}

void testClamp() {
    std::printf("clamp\n");
    {
        ScrollPhysics p;
        p.setBounds(0, 1000, 0);
        std::vector<Sample> all;
        for (int i = 0; i < 30; ++i) {
            p.wheel(-1, 120, i * 20.0);
            const auto s = run(p, i * 20.0, i * 20.0 + 19.9, kFrame60 / 2);
            all.insert(all.end(), s.begin(), s.end());
        }
        const auto tail = run(p, 600, 1500, kFrame60);
        all.insert(all.end(), tail.begin(), tail.end());
        float maxX = 0;
        for (const auto& s : all) maxX = std::max(maxX, s.x);
        CHECK(maxX <= 1000.f);
        CHECK(monotonic(all, 1));
        CHECK(p.position() == 1000.f && p.velocity() == 0.f && !p.moving());
        CHECK(!p.wheel(-1, 120, 1600));   // at the end: nothing to do
        CHECK(!p.moving());
    }
    {
        // A fast touchpad flick toward the end, then a slow follow glide onto the end: the slower spring would
        // overshoot (its velocity is too high for it); the bound stops it hard instead.
        ScrollPhysics p;
        p.setBounds(0, 1000, 0);
        p.snap(700);
        double t = 0;
        for (int i = 0; i < 12; ++i, t += 8) p.wheel(-0.2f, 120, t);   // 24 DIP every 8 ms: 3000 DIP/s
        p.advance(t);
        const float v = p.velocity();
        p.glideTo(1000, ScrollFeel::Follow, t);
        const auto s = run(p, t, t + 800, kFrame60);
        float maxX = 0;
        bool stoppedAtEnd = false;
        for (const auto& x : s) {
            maxX = std::max(maxX, x.x);
            if (x.x == 1000.f && x.v == 0.f) stoppedAtEnd = true;
        }
        std::printf("        flick at %.0f DIP/s into the end: max %.2f\n", v, maxX);
        CHECK(maxX <= 1000.f);
        CHECK(stoppedAtEnd);
        CHECK(!p.moving() && p.position() == 1000.f);
    }
    {
        ScrollPhysics p;
        p.setBounds(0, 1000, 0);
        p.snap(900);
        p.setBounds(0, 500, 0);   // content got shorter
        CHECK(p.position() == 500.f && p.target() == 500.f && !p.moving());
        // Moving fast toward 1000, the end comes in to 600 just ahead of it: lands there without passing it.
        p.setBounds(0, 1000, 0);
        p.snap(0);
        p.glideTo(1000, ScrollFeel::Glide, 0);
        p.advance(80);
        const float x = p.position();
        p.setBounds(0, x + 40, 80);
        const auto s = run(p, 80, 800, kFrame60);
        float maxX = 0;
        for (const auto& q : s) maxX = std::max(maxX, q.x);
        CHECK(maxX <= x + 40 + 1e-3f);
        CHECK(!p.moving() && std::abs(p.position() - (x + 40)) < 1e-3f);
    }
}

void testPrecision() {
    std::printf("precision\n");
    ScrollPhysics p;
    p.setBounds(0, 100000, 0);
    // A finger moving at ~2250 DIP/s: 18 DIP every 8 ms (0.15 notch), uneven like a real pad.
    double t = 0;
    float sum = 0, maxLag = 0;
    double nextFrame = kFrame60;
    for (int i = 0; i < 50; ++i, t += 8) {
        const float d = 0.15f + 0.02f * static_cast<float>((i * 7) % 3 - 1);
        p.wheel(-d, 120, t);
        sum += d * 120;
        while (nextFrame <= t + 8) {
            p.advance(nextFrame);
            if (nextFrame > 100) maxLag = std::max(maxLag, p.target() - p.position());
            nextFrame += kFrame60;
        }
    }
    CHECK(p.feel() == ScrollFeel::Precision);
    CHECK(p.precisionActive(t));
    CHECK(std::abs(p.target() - sum) < 0.01f);   // 1:1, no acceleration
    const float speed = sum / static_cast<float>(t / 1000.0);
    std::printf("        %.0f DIP/s: largest lag %.1f DIP (= %.1f ms)\n", speed, maxLag, maxLag / speed * 1000);
    CHECK(maxLag / speed * 1000 < kFrame60 * 1.3);
    const double stop = t;
    p.advance(stop + 60);
    CHECK(std::abs(p.target() - p.position()) < 1.f);
    run(p, stop + 60, stop + 300, kFrame60);
    CHECK(!p.moving());
    // Within the hold window a whole notch is still the pad (1:1); after it, a wheel notch (glide, may accelerate).
    const float before = p.target();
    p.wheel(-1, 120, stop + 300);
    CHECK(p.feel() == ScrollFeel::Precision && std::abs(p.target() - before - 120) < 1e-3f);
    run(p, stop + 300, stop + 1200, kFrame60);
    p.wheel(-1, 120, stop + 1200);
    CHECK(p.feel() == ScrollFeel::Wheel);
    // High-resolution wheel (8 deltas of 15 per notch): precision as well.
    ScrollPhysics h;
    h.setBounds(0, 10000, 0);
    for (int i = 0; i < 8; ++i) h.wheel(-0.125f, 120, i * 4.0);
    CHECK(h.feel() == ScrollFeel::Precision && std::abs(h.target() - 120.f) < 1e-3f);
}

// The same script at different frame timings: positions agree at the checkpoints.
std::vector<float> script(const std::function<double(double)>& nextFrame) {
    ScrollPhysics p;
    p.setBounds(0, 5000, 0);
    struct Input {
        double at;
        std::function<void(ScrollPhysics&, double)> fn;
    };
    const std::vector<Input> inputs = {
        {0, [](ScrollPhysics& q, double t) { q.wheel(-1, 120, t); }},
        {30, [](ScrollPhysics& q, double t) { q.wheel(-1, 120, t); }},
        {60, [](ScrollPhysics& q, double t) { q.wheel(-2, 120, t); }},
        {210, [](ScrollPhysics& q, double t) { q.wheel(1, 120, t); }},
        {400, [](ScrollPhysics& q, double t) { q.glideTo(2000, ScrollFeel::Glide, t); }},
        {650, [](ScrollPhysics& q, double t) { q.wheel(-0.3f, 120, t); }},
        {658, [](ScrollPhysics& q, double t) { q.wheel(-0.3f, 120, t); }},
    };
    const std::vector<double> checks = {100, 250, 500, 700, 1500};
    std::vector<float> out;
    size_t in = 0, ck = 0;
    double t = 0;
    while (ck < checks.size()) {
        const double frame = nextFrame(t);
        // Inputs and checkpoints between this frame and the next happen at their own times.
        while (in < inputs.size() && inputs[in].at <= frame) {
            inputs[in].fn(p, inputs[in].at);
            ++in;
        }
        while (ck < checks.size() && checks[ck] <= frame) {
            p.advance(checks[ck]);
            out.push_back(p.position());
            ++ck;
        }
        p.advance(frame);
        t = frame;
    }
    return out;
}

void testRate() {
    std::printf("rate\n");
    const auto a = script([](double t) { return t + kFrame60; });
    const auto b = script([](double t) { return t + 1000.0 / 144.0; });
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> gap(4, 40);
    const auto c = script([&](double t) { return t + gap(rng); });
    float worst = 0;
    for (size_t i = 0; i < a.size(); ++i) worst = std::max({worst, std::abs(a[i] - b[i]), std::abs(a[i] - c[i])});
    std::printf("        checkpoints %.2f %.2f %.2f %.2f %.2f, largest difference %.4f DIP\n", a[0], a[1], a[2],
                a[3], a[4], worst);
    CHECK(a.size() == 5 && b.size() == 5 && c.size() == 5);
    CHECK(worst < 0.25f);
}

void testKeys() {
    std::printf("keys\n");
    ScrollPhysics p;
    p.setBounds(0, 10000, 0);
    std::vector<Sample> all;
    for (int i = 0; i < 10; ++i) {
        p.glideTo(p.target() + 40, ScrollFeel::Glide, i * 33.0);
        const auto s = run(p, i * 33.0, i * 33.0 + 32.9, kFrame60 / 2);
        all.insert(all.end(), s.begin(), s.end());
    }
    const auto tail = run(p, 330, 1200, kFrame60);
    all.insert(all.end(), tail.begin(), tail.end());
    CHECK(monotonic(all, 1));
    CHECK(p.position() == 400.f && !p.moving());
}

void testFeels() {
    std::printf("feels\n");
    auto t95 = [](ScrollFeel f) {
        ScrollPhysics p;
        p.setBounds(0, 10000, 0);
        p.glideTo(1000, f, 0);
        return timeTo(run(p, 0, 2000, 1), 0, 1000, 0.95);
    };
    const double w = t95(ScrollFeel::Wheel), pr = t95(ScrollFeel::Precision), g = t95(ScrollFeel::Glide),
                 f = t95(ScrollFeel::Follow);
    std::printf("        95 %%: precision %.0f ms, wheel %.0f ms, glide %.0f ms, follow %.0f ms\n", pr, w, g, f);
    CHECK(pr < w && w < g && g < f);
    CHECK(pr <= 60);
    CHECK(g >= 150 && g <= 300);
    CHECK(f >= 350 && f <= 500);
}

void testInstant() {
    std::printf("instant\n");
    ScrollPhysics p;
    p.setBounds(0, 10000, 0);
    p.setInstant(true);
    p.wheel(-1, 120, 0);
    CHECK(p.position() == 120.f && !p.moving());
    p.glideTo(5000, ScrollFeel::Glide, 10);
    CHECK(p.position() == 5000.f && !p.moving());
    p.wheel(-0.1f, 120, 20);
    CHECK(std::abs(p.position() - 5012.f) < 1e-3f && !p.moving());
}

void testFrames() {
    std::printf("frames\n");
    frame::begin();
    (void)frame::consumeRequest();
    SmoothScroll s;
    s.setBounds(0, 10000);
    s.snap(100);
    CHECK(s.value() == 100.f);
    CHECK(!frame::consumeRequest());   // at rest: no frames
    s.glideTo(900);
    frame::begin();
    (void)s.value();
    CHECK(s.moving());
    CHECK(frame::consumeRequest());    // in flight: the next frame is asked for
    Sleep(900);
    frame::begin();
    CHECK(s.value() == 900.f);
    CHECK(!s.moving());
    CHECK(!frame::consumeRequest());   // settled: idle again
    motion::setReduced(true);
    s.glideTo(100);
    CHECK(s.value() == 100.f && !s.moving());
    motion::setReduced(false);
    // wheel::step follows Windows' "lines to scroll" (3 = the container's notch).
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const float step = wheel::step(120, 500);
    std::printf("        lines to scroll %u -> %.1f DIP a notch (page 500)\n", lines, step);
    if (lines == WHEEL_PAGESCROLL) CHECK(step == 500.f);
    else CHECK(std::abs(step - std::min(500.f, 40.f * static_cast<float>(lines))) < 1e-3f);
}

} // namespace

int main() {
    testNotch();
    testRetarget();
    testAccel();
    testReverse();
    testClamp();
    testPrecision();
    testRate();
    testKeys();
    testFeels();
    testInstant();
    testFrames();
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
