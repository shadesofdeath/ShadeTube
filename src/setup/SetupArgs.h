#pragma once
// ShadeTubeSetup.exe's command line (docs/DEVELOPMENT.md "Setup"). Switches are case-insensitive and start with '/'
// (or '-'):
//   /S            silent: no window, no questions; the exit code tells the result (package managers)
//   /D=<folder>   install folder (NSIS style: the last switch, may contain spaces, quotes optional)
//   /DESKTOP      also a desktop shortcut (the window offers it as an option)
//   /NOSTARTMENU  no Start menu shortcut
//   /AUTOSTART    start ShadeTube with Windows
//   /LAUNCH       start ShadeTube when done (silent mode; the window has it as an option)
//   /UNINSTALL    remove the installed copy (with /S: silent, the user data stays unless /REMOVEDATA)
//   /REMOVEDATA   with /UNINSTALL: also the settings, the Spotify session, the library and the caches
//   /? /HELP      the switches
// Dev flags (two dashes): --screenshot <folder> (every page as a PNG, then exit), --theme dark|light, --scale <f>
// (e.g. 1.5: the window at 150 % on any monitor), --lang <code>, --page <name> (open on that page; with
// --screenshot only that one).
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::setup {

// Exit codes (also in the docs): 0 done, 1 failed, 87 bad command line, 1602 cancelled by the user, 1618 another setup
// is running.
inline constexpr int kExitOk = 0, kExitFailed = 1, kExitBadArgs = 87, kExitCancelled = 1602, kExitAlreadyRunning = 1618;

struct Args {
    bool silent = false;
    std::wstring dir;   // /D=, as given (empty = default / the installed copy's folder)
    bool desktop = false, noStartMenu = false, autostart = false, launch = false;
    bool uninstall = false, removeData = false;
    bool help = false;
    // dev
    std::wstring screenshotDir;
    std::optional<bool> light;
    float scale = 0;
    std::string lang;
    std::string page;
};

// Parses everything after the program name of a raw command line (GetCommandLineW() without the exe). nullopt + an
// error text (the offending switch) when something is unknown or malformed.
std::optional<Args> parseArgs(std::wstring_view args, std::wstring* error = nullptr);
// The part of GetCommandLineW() after the program name (quoted or not).
std::wstring_view argumentsOf(std::wstring_view commandLine);

} // namespace st::setup
