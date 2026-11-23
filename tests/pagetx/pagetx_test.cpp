// Developer test for page transitions (ui/PageMotion), stepped with explicit times (deterministic), offline:
//   durations  the three timelines last 180..240 ms; a slow-motion scale stretches them; done = not running
//   push       outgoing fades out (monotonic, gone at 40 %) shifting left; incoming slides in from the right
//              (monotonic, decelerating) and fades in after 30 %; both end exactly (identity / gone)
//   back       the mirror image of push: same magnitudes, opposite directions
//   shared     with a flying cover the pages only cross-fade (no offsets); replace fades in place
//   fly        the cover lands exactly on the target (rect + radius), approaches it monotonically, never overshoots,
//              the target paints nothing meanwhile; a target that moves mid-flight is still hit exactly
//   land       an image that is ready: 120 ms hand-over (the cover fades out while the target's presence rises), idle
//   hold       not ready: the cover stays on the target without asking for frames until the image arrives, the
//              target moves (scrolled) or 1.5 s pass, then lands; the wake-up time is the hold's end
//   fade       a target that disappears (or never appears) dissolves the cover in place in 120 ms
//   rate       60 Hz and 144 Hz stepping give the same rect at the same time
//   pagetx_test
#include "ui/Anim.h"
#include "ui/PageMotion.h"

#include <cmath>
#include <cstdio>
#include <optional>

using namespace st::ui;
using st::gfx::Rect;

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

bool approx(float a, float b, float eps = 1e-3f) { return std::abs(a - b) <= eps; }
bool sameRect(const Rect& a, const Rect& b, float eps = 1e-3f) {
    return approx(a.x, b.x, eps) && approx(a.y, b.y, eps) && approx(a.w, b.w, eps) && approx(a.h, b.h, eps);
}
float dist(const Rect& a, const Rect& b) {
    return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.w - b.w) + std::abs(a.h - b.h);
}

constexpr double kFrame = 1000.0 / 144.0;

void durations() {
    std::printf("durations\n");
    PageMotion m;
    m.start(PageNav::Push, false, 0);
    CHECK(m.duration() >= 180 && m.duration() <= 240);
    CHECK(m.running(100) && !m.running(m.duration() + 0.1));
    m.start(PageNav::Back, true, 0);
    CHECK(m.shared() && m.duration() >= 180 && m.duration() <= 240);
    m.start(PageNav::Replace, true, 0);
    CHECK(!m.shared() && m.duration() >= 180 && m.duration() <= 240);
    m.start(PageNav::Push, false, 1000, 10);
    CHECK(approx(static_cast<float>(m.duration()), static_cast<float>(PageMotion::kSlideMs * 10)));
    m.stop();
    CHECK(!m.running(1001));
    CHECK(m.incoming(1001).identity());
    CHECK(m.outgoing(1001).alpha == 0);
}

void push() {
    std::printf("push\n");
    PageMotion m;
    m.start(PageNav::Push, false, 0);
    const double d = m.duration();
    CHECK(approx(m.outgoing(0).alpha, 1) && approx(m.outgoing(0).dx, 0));
    CHECK(approx(m.incoming(0).alpha, 0) && approx(m.incoming(0).dx, PageMotion::kInShift));
    bool outMono = true, inMono = true, inAlphaMono = true, outLeft = true, inFromRight = true;
    float pa = 2, pdx = 1, pia = -1, pidx = 1e9f;
    for (double t = 0; t <= d; t += kFrame) {
        const PageLayer o = m.outgoing(t), i = m.incoming(t);
        outMono &= o.alpha <= pa + 1e-6f && o.dx <= pdx + 1e-6f;
        outLeft &= o.dx <= 0;
        inMono &= i.dx <= pidx + 1e-6f;
        inFromRight &= i.dx >= 0;
        inAlphaMono &= i.alpha >= pia - 1e-6f;
        pa = o.alpha, pdx = o.dx, pia = i.alpha, pidx = i.dx;
    }
    CHECK(outMono && outLeft);
    CHECK(inMono && inFromRight && inAlphaMono);
    CHECK(approx(m.outgoing(d * 0.4).alpha, 0) && m.outgoingDone(d * 0.41) && !m.outgoingDone(d * 0.3));
    CHECK(approx(m.outgoing(d * 0.4).dx, -PageMotion::kOutShift));
    CHECK(approx(m.incoming(d * 0.3).alpha, 0));   // fades in once the outgoing page has mostly gone
    CHECK(m.incoming(d * 0.5).dx < PageMotion::kInShift * 0.25f);   // decelerating: most of the way at half time
    CHECK(m.incoming(d).identity());
}

void back() {
    std::printf("back\n");
    PageMotion p, b;
    p.start(PageNav::Push, false, 0);
    b.start(PageNav::Back, false, 0);
    bool mirrored = true;
    for (double t = 0; t <= p.duration(); t += kFrame) {
        mirrored &= approx(p.outgoing(t).dx, -b.outgoing(t).dx) && approx(p.incoming(t).dx, -b.incoming(t).dx);
        mirrored &= approx(p.outgoing(t).alpha, b.outgoing(t).alpha) && approx(p.incoming(t).alpha, b.incoming(t).alpha);
    }
    CHECK(mirrored);
    CHECK(b.incoming(0).dx < 0 && b.outgoing(b.duration() * 0.3).dx > 0);
}

void sharedAndReplace() {
    std::printf("shared / replace\n");
    PageMotion m;
    m.start(PageNav::Push, true, 0);
    bool still = true;
    for (double t = 0; t <= m.duration(); t += kFrame) still &= m.outgoing(t).dx == 0 && m.incoming(t).dx == 0;
    CHECK(still);
    CHECK(approx(m.outgoing(m.duration() * 0.4).alpha, 0));
    CHECK(m.incoming(m.duration()).identity());
    m.start(PageNav::Replace, false, 0);
    still = true;
    for (double t = 0; t <= m.duration(); t += kFrame) still &= m.incoming(t).dx == 0;
    CHECK(still && approx(m.incoming(0).alpha, 0) && m.incoming(m.duration()).identity());
}

void fly() {
    std::printf("fly\n");
    const Rect from{100, 500, 150, 150}, to{288, 76, 232, 232};
    CoverFlight f;
    f.start(from, 75, 0);   // a round artist photo ...
    CHECK(f.active() && f.phase() == CoverFlight::Phase::Fly && f.targetPresence() == 0);
    float prev = 1e9f;
    bool approach = true, inside = true;
    for (double t = 0; t < CoverFlight::kFlyMs; t += kFrame) {
        f.update(t, CoverFlight::Target{to, 2, true});
        const float dd = dist(f.rect(), to);
        approach &= dd <= prev + 1e-3f;
        prev = dd;
        // never past the target: x / y between the two ends
        inside &= f.rect().x >= from.x - 1e-3f && f.rect().x <= to.x + 1e-3f && f.rect().y <= from.y + 1e-3f &&
                  f.rect().y >= to.y - 1e-3f;
        approach &= f.targetPresence() == 0 && f.alpha() == 1 && f.animating();
    }
    CHECK(approach && inside);
    f.update(CoverFlight::kFlyMs, CoverFlight::Target{to, 2, true});
    CHECK(sameRect(f.rect(), to) && approx(f.radius(), 2));   // ... into a square header, exactly
    CHECK(f.phase() == CoverFlight::Phase::Land);

    // A target that moves mid-flight (the page scrolled, its header got built) is still hit exactly.
    CoverFlight g;
    g.start(from, 2, 0);
    g.update(60, CoverFlight::Target{to, 2, true});
    const Rect moved = to.offset(0, -40);
    for (double t = 60; t <= CoverFlight::kFlyMs; t += kFrame) g.update(t, CoverFlight::Target{moved, 2, true});
    g.update(CoverFlight::kFlyMs, CoverFlight::Target{moved, 2, true});
    CHECK(sameRect(g.rect(), moved));
    // Halfway: the radius is between the two ends.
    CoverFlight h;
    h.start(from, 75, 0);
    h.update(CoverFlight::kFlyMs * 0.5, CoverFlight::Target{to, 2, true});
    CHECK(h.radius() < 75 && h.radius() > 2);
}

void land() {
    std::printf("land\n");
    const Rect from{10, 10, 40, 40}, to{300, 80, 232, 232};
    CoverFlight f;
    f.start(from, 2, 0);
    f.update(CoverFlight::kFlyMs, CoverFlight::Target{to, 2, true});
    CHECK(f.phase() == CoverFlight::Phase::Land && approx(f.alpha(), 1) && approx(f.targetPresence(), 0));
    float pa = 2, pp = -1;
    bool mono = true;
    for (double t = CoverFlight::kFlyMs; t < CoverFlight::kFlyMs + CoverFlight::kLandMs; t += kFrame) {
        f.update(t, CoverFlight::Target{to, 2, true});
        mono &= f.alpha() <= pa + 1e-6f && f.targetPresence() >= pp - 1e-6f && sameRect(f.rect(), to);
        pa = f.alpha(), pp = f.targetPresence();
    }
    CHECK(mono);
    f.update(CoverFlight::kFlyMs + CoverFlight::kLandMs, CoverFlight::Target{to, 2, true});
    CHECK(!f.active() && f.alpha() == 0 && f.targetPresence() == 1 && !f.animating());
}

void hold() {
    std::printf("hold\n");
    const Rect from{10, 10, 40, 40}, to{300, 80, 232, 232};
    const double end = CoverFlight::kFlyMs;
    CoverFlight f;
    f.start(from, 2, 0);
    f.update(end, CoverFlight::Target{to, 2, false});   // the header's image is still loading
    CHECK(f.phase() == CoverFlight::Phase::Hold && !f.animating() && f.alpha() == 1 && f.targetPresence() == 0);
    CHECK(approx(static_cast<float>(f.wakeAt()), static_cast<float>(end + CoverFlight::kHoldMaxMs)));
    f.update(end + 400, CoverFlight::Target{to, 2, false});
    CHECK(f.phase() == CoverFlight::Phase::Hold && sameRect(f.rect(), to));
    f.update(end + 500, CoverFlight::Target{to, 2, true});   // arrived
    CHECK(f.phase() == CoverFlight::Phase::Land);

    CoverFlight g;   // the user scrolled the header away from under it
    g.start(from, 2, 0);
    g.update(end, CoverFlight::Target{to, 2, false});
    g.update(end + 100, CoverFlight::Target{to.offset(0, -30), 2, false});
    CHECK(g.phase() == CoverFlight::Phase::Land);

    CoverFlight h;   // it never arrives: the target shows what it has after the hold
    h.start(from, 2, 0);
    h.update(end, CoverFlight::Target{to, 2, false});
    h.update(end + CoverFlight::kHoldMaxMs - 1, CoverFlight::Target{to, 2, false});
    CHECK(h.phase() == CoverFlight::Phase::Hold);
    h.update(end + CoverFlight::kHoldMaxMs, CoverFlight::Target{to, 2, false});
    CHECK(h.phase() == CoverFlight::Phase::Land);
}

void fade() {
    std::printf("fade\n");
    const Rect from{10, 10, 40, 40}, to{300, 80, 232, 232};
    CoverFlight f;
    f.start(from, 2, 0);
    f.update(100, CoverFlight::Target{to, 2, true});
    const Rect where = f.rect();
    f.update(110, std::nullopt);   // rebuilt into an error panel / gone
    CHECK(f.phase() == CoverFlight::Phase::Fade && f.targetPresence() == 1 && sameRect(f.rect(), where));
    float pa = 2;
    bool mono = true;
    for (double t = 110; t < 110 + CoverFlight::kLandMs; t += kFrame) {
        f.update(t, std::nullopt);
        mono &= f.alpha() <= pa + 1e-6f && sameRect(f.rect(), where);
        pa = f.alpha();
    }
    CHECK(mono);
    f.update(110 + CoverFlight::kLandMs, std::nullopt);
    CHECK(!f.active());

    CoverFlight g;   // never there at all
    g.start(from, 2, 0);
    g.update(0, std::nullopt);
    CHECK(g.phase() == CoverFlight::Phase::Fade && sameRect(g.rect(), from));
    CoverFlight hld;   // gone during a hold
    hld.start(from, 2, 0);
    hld.update(CoverFlight::kFlyMs, CoverFlight::Target{to, 2, false});
    hld.update(CoverFlight::kFlyMs + 50, std::nullopt);
    CHECK(hld.phase() == CoverFlight::Phase::Fade);
}

void rate() {
    std::printf("rate\n");
    const Rect from{10, 600, 150, 150}, to{300, 80, 232, 232};
    CoverFlight a, b;
    a.start(from, 2, 0);
    b.start(from, 2, 0);
    for (double t = 0; t <= 120; t += 1000.0 / 60) a.update(t, CoverFlight::Target{to, 2, true});
    for (double t = 0; t <= 120; t += 1000.0 / 144) b.update(t, CoverFlight::Target{to, 2, true});
    a.update(120, CoverFlight::Target{to, 2, true});
    b.update(120, CoverFlight::Target{to, 2, true});
    CHECK(sameRect(a.rect(), b.rect()));
    CHECK(a.flyProgress() > 0.6f);   // Standard easing: well past half way at half time
}

} // namespace

int main() {
    durations();
    push();
    back();
    sharedAndReplace();
    fly();
    land();
    hold();
    fade();
    rate();
    std::printf(g_failures ? "\n%d FAILED\n" : "\nall passed\n", g_failures);
    return g_failures ? 1 : 0;
}
