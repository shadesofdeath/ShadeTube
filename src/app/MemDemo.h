#pragma once
// SHADETUBE_MEM_DEMO (dev): a scripted session for memory measurements, so every scenario of docs/DEVELOPMENT.md 3.2
// can be measured the same way again. The value is `<action>@<ms>[=<arg>];...` (each run `<ms>` after startup) or the
// path of a file holding such steps (separated by `;` or new lines, `#` starts a comment line). Actions:
//   route=<r>       navigate (the --route forms: home, search:<q>, album:<id>, artist:<id>, playlist:<id>, liked,
//                   library[:<view>], stats, local[:<view>], settings[:<card>], radio, podcasts, releases, smart:<id>,
//                   downloads)
//   back, forward   history
//   wheel=<n>       n wheel notches down (negative: up) over the page, 60 ms apart
//   np, npclose     Now Playing open / closed;  next  the next song
//   tray, show      the main window hidden to the tray / shown again;  minimize, restore
//   mini, miniclose the mini player opened (the main window hides) / closed (the main window comes back)
//   trim            what App does when the window goes away (App::trimMemory)
//   gputrim, gpuclear  Device::trimPool() / Direct2D's ClearResources(0) alone (to weigh each part)
//   d2dcache=<MB>   Direct2D's texture cache budget (SetMaximumTextureMemory) from now on
//   report[=<label>] a categorized memory report (core/MemoryStats + the app's caches, plus the heap profile of
//                   ST_MEMPROFILE builds) appended to logs\memreport.txt; one summary line in the log
//   frames=<label>  only the frame times since the last report / frames step (a transition's frames, say)
//   quit            exit normally (flushes the log)
#include <functional>

namespace st::ui {
class Window;
}

namespace st::app::memdemo {

struct Hooks {
    std::function<void()> tray, show, mini, miniClose, trim, quit;
    std::function<ui::Window*()> miniWindow;   // the mini player's window (nullptr without one)
};

void schedule(Hooks hooks);
// The main window drew a frame that took `cpuMs` (render start to present). Reports show the frames since the previous
// one (count, mean, 95th percentile, slowest) and the first frame after each gputrim / gpuclear / trim step.
void noteFrame(double cpuMs);

} // namespace st::app::memdemo
