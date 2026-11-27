#pragma once
// ShadeTubeSetup.exe's window: a borderless 800 × 560 DIP ui::Window drawn with the app's own gfx / ui libraries
// (fonts, tokens, buttons, toggles, focus ring, motion). Left: the record (the logo mark inside a ring of dial ticks
// that light up with the progress; it spins while the files are written); right: one page at a time.
//   welcome      fresh install: folder ("Değiştir…"), options, "Kur"
//   maintenance  an installed copy: update / repair / downgrade, "Kaldır…"
//   uninstall    "Ayarlarımı ve kitaplığımı sakla", "Kaldır"
//   progress     percentage, the player bar's wave line as the progress bar, the steps ticking off
//   done         "ShadeTube'u aç" / "Kapat" (or the app opens by itself when "Bitince ShadeTube'u aç" was on)
//   error        the reason, "Tekrar dene"
// Enter = the page's main action, Esc = back / close, Tab / Shift+Tab move the focus (ring after keyboard use); the
// whole window drags by its empty areas. The work runs on a worker thread (SetupEngine::run).
#include "setup/Payload.h"
#include "setup/SetupArgs.h"
#include "setup/SetupEngine.h"

#include <string>
#include <string_view>

namespace st::setup {

// Runs the window until it closes; returns the exit code (kExitOk, kExitFailed, kExitCancelled). A payload that
// failed read() comes with `payloadError` (the window opens on the error page).
int runWindow(const Args& args, std::string_view payload, const payload::Info& info, const std::wstring& payloadError);
// Dev (--screenshot <folder>): every page (or --page's) in the current theme / scale as <folder>\<page>.png, with
// made-up states (nothing is installed or read beyond the payload); returns the number of failed captures.
int screenshots(const Args& args, std::string_view payload, const payload::Info& info);

} // namespace st::setup
