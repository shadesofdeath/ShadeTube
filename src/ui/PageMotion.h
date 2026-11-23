#pragma once
// Page transitions, the arithmetic part (docs/DEVELOPMENT.md, "Page transitions"): how the outgoing and the incoming
// page move and fade over one page change, and the shared-element flight of a cover from where it was clicked to the
// header of the page it opens (and back). Standalone like ScrollPhysics: every time is passed in (ms, the frame clock in
// the app), so pagetx_test steps it with explicit times. app/PageTransition puts it on the screen.
#include "gfx/Types.h"

#include <optional>

namespace st::ui {

using gfx::Rect;

enum class PageNav {
    Push,      // a new page (or Forward through the history): the incoming page comes from the right
    Back,      // back through the history: everything mirrored, the incoming page comes from the left
    Replace,   // the first page, a redirect: the incoming page fades in in place
};

// How one page is drawn at a moment of the transition: group opacity and an offset in DIPs.
struct PageLayer {
    float alpha = 1, dx = 0, dy = 0;
    bool identity() const { return alpha >= 0.999f && dx == 0 && dy == 0; }
};

// Timeline of one page change. Push / Back: the outgoing page fades out over the first 45 % while it shifts 12 DIP the
// other way (accelerating), the incoming page slides 24 DIP in (decelerating over the whole 220 ms) and fades in after the
// first 20 %. With a shared cover the pages only cross-fade (the cover is what moves) over the flight's 240 ms. Replace:
// a 180 ms fade in place.
class PageMotion {
public:
    static constexpr double kSlideMs = 220, kSharedMs = 240, kReplaceMs = 180;
    static constexpr float kInShift = 24, kOutShift = 12;

    // `scale` > 1 stretches every duration (dev: SHADETUBE_MOTION_SCALE, slow motion for screenshots).
    void start(PageNav nav, bool shared, double now, double scale = 1);
    void stop() { running_ = false; }
    bool running(double now) const { return running_ && now < start_ + duration_; }
    double duration() const { return duration_; }
    PageNav nav() const { return nav_; }
    bool shared() const { return shared_; }

    PageLayer outgoing(double now) const;   // alpha 0 once it is gone (from 45 % on)
    PageLayer incoming(double now) const;   // identity once done
    bool outgoingDone(double now) const { return !running_ || progress(now) >= outEnd(); }

private:
    float progress(double now) const;       // 0..1 linear
    float outEnd() const { return shared_ || nav_ == PageNav::Replace ? 0.4f : 0.45f; }

    PageNav nav_ = PageNav::Replace;
    bool shared_ = false, running_ = false;
    double start_ = 0, duration_ = 0;
};

// The flying cover. Fly: from the source rect to the target (re-read every frame: it may scroll, load or go), eased
// (Standard, 240 ms), the corner radius morphing with it (a round artist photo into a square header). Then the target's
// own artwork takes over: Land (120 ms) when the target already paints its image (ready), else Hold - the cover stays on
// the target, following it, until the image is there (at most 1.5 s, or until the target moves away, e.g. scrolled),
// then Land. A target that disappears (or never shows) ends it with Fade: the cover dissolves where it is (120 ms).
class CoverFlight {
public:
    static constexpr double kFlyMs = 240, kLandMs = 120, kHoldMaxMs = 1500;
    enum class Phase { Idle, Fly, Hold, Land, Fade };
    struct Target {
        Rect rect;
        float radius = 2;
        bool ready = true;   // the target's own artwork is loaded (it would paint the image now)
    };

    void start(const Rect& from, float fromRadius, double now, double scale = 1);
    void cancel() { phase_ = Phase::Idle; }
    // Advances to `now` with the target as it is now (nullopt: none / gone).
    void update(double now, const std::optional<Target>& target);

    Phase phase() const { return phase_; }
    bool active() const { return phase_ != Phase::Idle; }
    // What to draw: the cover's rect / radius and opacity (0 = nothing).
    Rect rect() const { return rect_; }
    float radius() const { return radius_; }
    float alpha() const { return alpha_; }
    // How much the target paints its own cover: 0 while the flight shows it (Fly / Hold), rising while it lands, 1 when
    // it is not the flight's (Idle, Fade). The source never paints its cover while the flight is active.
    float targetPresence() const { return presence_; }
    // Needs another frame (Fly / Land / Fade move every frame; Hold waits for an image or its timeout).
    bool animating() const { return phase_ == Phase::Fly || phase_ == Phase::Land || phase_ == Phase::Fade; }
    // Hold: when its timeout ends it (a wake-up for an idle window); +inf otherwise.
    double wakeAt() const;
    // Fly: eased progress 0..1 (the target rect it lerps to was the one passed last).
    float flyProgress() const { return flyE_; }

private:
    void land(double now);
    void fade(double now);

    Phase phase_ = Phase::Idle;
    Rect from_{}, rect_{}, landed_{};
    float fromRadius_ = 0, radius_ = 0, alpha_ = 0, presence_ = 1, flyE_ = 0;
    double start_ = 0, phaseStart_ = 0, scale_ = 1;
    bool seenTarget_ = false;
    float fadeFrom_ = 1;
};

// Linear interpolation of rects (x, y, w, h).
Rect lerpRect(const Rect& a, const Rect& b, float t);

} // namespace st::ui
