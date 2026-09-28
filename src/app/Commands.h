#pragma once
// What the keyboard shortcuts do (app side of app/Shortcuts): the handlers of the actions, the key dispatch of the
// main window and the mini player, the global hotkeys (RegisterHotKey on the main window) and the startup work of
// the system features (binding cleanup, the "Windows ile başlat" Run value). UI thread only.
#include "app/Shortcuts.h"
#include "ui/Widget.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace st::app::commands {

enum class Scope { Main, Mini };

// Runs an action now. False when it can't do anything at the moment (e.g. the lyrics view isn't available), so the
// key press may still go elsewhere.
bool run(std::string_view id);
// App-provided handlers for what only App can do (its windows, the mini player, Now Playing). Override the
// built-in ones; cleared by clearHandlers() before App goes away.
void setHandler(std::string_view id, std::function<bool()> fn);
void clearHandlers();
// A key press no widget took (Window::onKey). `inTextField`: a text field has focus, only kInText actions run.
// Auto-repeat runs only kRepeat actions (the others swallow it, so holding Ctrl+L doesn't flip the heart 20 times).
bool dispatchKey(const ui::KeyEvent& e, Scope scope, bool inTextField);

// ---- Global hotkeys (Settings::shortcuts "global:<id>") -------------------------------------------------------------
void initGlobalHotkeys(HWND hwnd);   // App, once the main window exists (it lives for the whole run)
// Re-registers every global binding (after a change in Ayarlar). A combo another app already holds fails:
// globalHotkeyFailed() tells which.
void applyGlobalHotkeys();
bool globalHotkeyFailed(std::string_view id);
bool handleHotkey(WPARAM id);        // WM_HOTKEY; true when it was one of ours
void shutdownGlobalHotkeys();

// ---- System features --------------------------------------------------------------------------------------------------
// App startup: drops invalid / duplicate bindings and keeps the Run value in line with Settings::startWithWindows.
void initSystemFeatures();
// The exe Windows should start at sign-in: the installed copy when there is one, else the running exe.
std::filesystem::path autostartExe();

} // namespace st::app::commands
