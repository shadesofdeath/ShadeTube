#pragma once
// Press-and-hold detection (song preview, app/Preview). Standalone: no window, times are passed in. ui::Window drives
// it with the mouse messages and a timer; tests/preview steps it with explicit times.
//
//   press()  a plain left press a widget took                        -> Pending
//   move()   the pointer travelled past `slop` from the press         -> Idle (a click / drag goes on as before)
//   due()    Pending for `delayMs`: the window asks the app now; take() makes the rest of the gesture the hold's
//   finish() the hold ended before the release (Esc, its widget went away): the release is still swallowed
//   release() true when the release belongs to the hold (the widget must not see it as a click)
//   cancel() another button, the capture was lost, the app declined   -> Idle
#include "gfx/Types.h"

namespace st::ui {

class HoldGesture {
public:
    enum class State { Idle, Pending, Taken, Done };

    double delayMs = 350;   // how long the press must rest
    float slop = 6;         // DIPs it may travel meanwhile (the drag threshold)

    State state() const { return state_; }
    bool pending() const { return state_ == State::Pending; }
    bool taken() const { return state_ == State::Taken; }
    bool owned() const { return state_ == State::Taken || state_ == State::Done;   }   // the gesture is the hold's
    gfx::Point origin() const { return origin_; }

    void press(gfx::Point p, double nowMs);
    // A pointer move: a pending hold that travelled past the slop is cancelled. Returns true while it stays pending.
    bool move(gfx::Point p);
    bool due(double nowMs) const { return state_ == State::Pending && nowMs - pressedAt_ >= delayMs; }
    double remainingMs(double nowMs) const;   // until due (0 when due or not pending)
    void take();                              // Pending -> Taken
    void finish();                            // Taken -> Done
    bool release();                           // -> Idle; true when the hold owned the gesture
    void cancel() { state_ = State::Idle; }

private:
    State state_ = State::Idle;
    gfx::Point origin_{};
    double pressedAt_ = 0;
};

} // namespace st::ui
