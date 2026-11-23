// Developer test for the micro-interactions (ui/Micro), offline, no window or device:
//   gate       motion::micro() = reduce motion off and Windows' animation effects on; microMs() collapses to 0
//   hover      HoverFade: the hovered row fades in over ~120 ms, the row left fades out from where it was, going back
//              resumes without a jump, clear() is instant, no frames are asked for once settled; instant without micro
//   clock      the equalizer clock advances 1:1 while playing, slows to a stop within ~240 ms when paused and then
//              stays frozen (no frames asked), picks up again on resume, caps long gaps at 100 ms; instant without micro
//   levels     four bars in 0.08..1, never all equal, moving over time; a fixed pose without micro
//   heart      HeartPop: like = pop to <= 116 % and back, the fill grows monotonically, the burst runs, settled after
//              220 ms; unlike = a dip below 100 % while the fill shrinks; another state or no micro = no animation
//   card       the card rise and press scale
//   micro_test
// Anim starts its fades at the real clock, so the timed checks wait for real time (busy-waits, ~2 s in total).
#include "ui/Anim.h"
#include "ui/Micro.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

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

// Waits (spinning) until the real clock reaches `ms`, then starts a frame there.
void frameAt(double ms) {
    while (frame::realNow() < ms) {
    }
    frame::begin();
}

bool approx(float a, float b, float tol) { return std::abs(a - b) <= tol; }

void gate() {
    std::printf("gate\n");
    motion::setReduced(false);
    motion::setSystemAnimations(true);
    CHECK(motion::micro());
    CHECK(motion::microMs(120) == 120);
    motion::setSystemAnimations(false);
    CHECK(!motion::micro());
    CHECK(motion::microMs(120) == 0);
    motion::setSystemAnimations(true);
    motion::setReduced(true);
    CHECK(!motion::micro());
    motion::setReduced(false);
}

void hover() {
    std::printf("hover\n");
    HoverFade f;
    frame::begin();
    CHECK(f.of(3) == 0.f && f.index() == -1);
    const double t0 = frame::realNow();
    f.set(3);
    frameAt(t0 + 60);
    const float mid = f.of(3);
    CHECK(mid > 0.2f && mid < 0.98f);   // standard easing: ~0.9 at half time, never a jump to 1
    CHECK(f.of(4) == 0.f);
    frameAt(t0 + 140);
    CHECK(f.of(3) == 1.f);
    // Down to row 4: 3 fades out from 1, 4 fades in.
    const double t1 = frame::realNow();
    f.set(4);
    frameAt(t1 + 40);
    const float out3 = f.of(3), in4 = f.of(4);
    CHECK(out3 < 1.f && out3 > 0.f);
    CHECK(in4 > 0.f && in4 < 1.f);
    // Back onto 3 mid-fade: it resumes from where it was (no drop to 0).
    const float before = f.of(3);
    f.set(3);
    CHECK(approx(f.of(3), before, 0.08f));
    frameAt(frame::realNow() + 150);
    CHECK(f.of(3) == 1.f && f.of(4) == 0.f);
    frame::consumeRequest();
    (void)f.of(3);
    CHECK(!frame::consumeRequest());   // settled: nothing asks for frames
    CHECK(!f.running());
    f.clear();
    CHECK(f.of(3) == 0.f && f.index() == -1);
    // Without micro-interactions the highlight switches at once.
    motion::setSystemAnimations(false);
    HoverFade g;
    g.set(2);
    CHECK(g.of(2) == 1.f);
    g.set(5);
    CHECK(g.of(2) == 0.f && g.of(5) == 1.f);
    motion::setSystemAnimations(true);
}

void clock() {
    std::printf("clock\n");
    equalizer::Clock c;
    double now = 1000;
    c.advance(now, true);
    for (int i = 0; i < 30; ++i) c.advance(now += 25, true);
    CHECK(std::abs(c.time() - 750) < 1e-6);   // 1:1 while playing
    CHECK(c.moving());
    // Pause: slows to a stop within motion::medium (240 ms), less than 240 ms of clock time pass.
    const double atPause = c.time();
    c.advance(now += 25, false);
    double prev = c.time(), prevStep = 25;
    bool slowing = true;
    for (int i = 0; i < 12; ++i) {
        c.advance(now += 25, false);
        const double step = c.time() - prev;
        if (step > prevStep + 1e-6) slowing = false;
        prev = c.time();
        prevStep = step;
    }
    CHECK(slowing);
    CHECK(c.speed() == 0.f && !c.moving());
    CHECK(c.time() - atPause > 25 && c.time() - atPause < 240);
    // Frozen: time stands still, no frames asked for.
    const double frozen = c.time();
    frame::consumeRequest();
    for (int i = 0; i < 10; ++i) c.advance(now += 25, false);
    CHECK(c.time() == frozen);
    CHECK(!frame::consumeRequest());
    // Resume: picks up again (the ramp asks for frames), then 1:1.
    c.advance(now += 25, true);
    c.advance(now += 25, true);
    CHECK(frame::consumeRequest());
    for (int i = 0; i < 12; ++i) c.advance(now += 25, true);
    const double t = c.time();
    c.advance(now += 25, true);
    CHECK(std::abs(c.time() - t - 25) < 1e-6);
    // A long gap (the bars were off screen) counts as at most 100 ms.
    const double beforeGap = c.time();
    c.advance(now += 5000, true);
    CHECK(std::abs(c.time() - beforeGap - 100) < 1e-6);
    // Two indicators in one frame: the second call adds nothing.
    const double once = c.time();
    c.advance(now, true);
    CHECK(c.time() == once);
    // Without micro-interactions a pause stops at once.
    motion::setSystemAnimations(false);
    c.advance(now += 25, false);
    CHECK(c.speed() == 0.f && !c.moving());
    motion::setSystemAnimations(true);
}

void levels() {
    std::printf("levels\n");
    float a[4], b[4];
    bool inRange = true, varied = true, moves = false;
    for (double t = 0; t < 4000; t += 33) {
        equalizer::levels(t, a);
        equalizer::levels(t + 66, b);
        for (int i = 0; i < 4; ++i) {
            inRange = inRange && a[i] >= 0.08f && a[i] <= 1.f;
            moves = moves || std::abs(a[i] - b[i]) > 0.05f;
        }
        const float lo = *std::min_element(a, a + 4), hi = *std::max_element(a, a + 4);
        varied = varied && hi - lo > 0.01f;
    }
    CHECK(inRange);
    CHECK(varied);
    CHECK(moves);
    motion::setSystemAnimations(false);
    equalizer::levels(0, a);
    equalizer::levels(1234, b);
    CHECK(std::equal(a, a + 4, b));   // still pose
    motion::setSystemAnimations(true);
}

void heart() {
    std::printf("heart\n");
    HeartPop p;
    frame::begin();
    CHECK(!p.running());
    CHECK(p.pose(true).fill == 1.f && p.pose(true).scale == 1.f && p.pose(true).burst < 0);
    CHECK(p.pose(false).fill == 0.f && p.pose(false).outline == 1.f);
    const double t0 = frame::realNow();
    p.start(true);
    float maxScale = 1, lastFill = 0;
    bool fillUp = true, burst = false;
    for (double t = 10; t < HeartPop::kLikeMs - 5; t += 10) {
        frameAt(t0 + t);
        const auto q = p.pose(true);
        maxScale = std::max(maxScale, q.scale);
        if (q.fill + 1e-4f < lastFill) fillUp = false;
        lastFill = q.fill;
        burst = burst || (q.burst > 0.2f && q.burst < 1.f);
        CHECK(q.scale >= 1.f && q.scale <= 1.161f);
    }
    CHECK(maxScale > 1.1f);
    CHECK(fillUp);
    CHECK(burst);
    CHECK(p.pose(false).fill == 0.f);   // the other state is not animated
    frameAt(t0 + HeartPop::kLikeMs + 5);
    CHECK(!p.running());
    CHECK(p.pose(true).scale == 1.f && p.pose(true).fill == 1.f && p.pose(true).burst < 0);
    // Unlike: a dip, the fill shrinks.
    const double t1 = frame::realNow();
    p.start(false);
    frameAt(t1 + HeartPop::kUnlikeMs * 0.5);
    const auto u = p.pose(false);
    CHECK(u.scale < 1.f && u.scale > 0.9f);
    CHECK(u.fill > 0.f && u.fill < 1.f);
    CHECK(u.burst < 0);
    frameAt(t1 + HeartPop::kUnlikeMs + 5);
    CHECK(!p.running() && p.pose(false).fill == 0.f);
    frame::consumeRequest();
    (void)p.pose(false);
    CHECK(!frame::consumeRequest());
    // No micro-interactions: nothing runs.
    motion::setReduced(true);
    p.start(true);
    CHECK(!p.running());
    motion::setReduced(false);
}

void cards() {
    std::printf("card\n");
    CHECK(card::lift(0, 0) == 0.f);
    CHECK(card::lift(1, 0) == -2.f);
    CHECK(card::lift(1, 1) == 0.f);
    CHECK(card::pressScale(0) == 1.f);
    CHECK(approx(card::pressScale(1), 0.985f, 1e-6f));
}

} // namespace

int main() {
    motion::setSystemAnimations(true);
    gate();
    hover();
    clock();
    levels();
    heart();
    cards();
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
