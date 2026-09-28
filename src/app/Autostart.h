#pragma once
// "Windows ile başlat": the HKCU Run value that starts ShadeTube at sign-in (`"<exe>" --autostart`).
//
// Windows' own switch for it (Task Manager › Startup apps, Settings › Apps › Startup) is a separate value under
// Explorer\StartupApproved\Run: disabledByWindows() reads it, and turning the option on in Ayarlar clears it so the
// user's latest choice wins. At startup the value is kept pointing at the exe that should start (the installed copy
// when there is one, else the running one), so a moved portable copy or an install keeps working.
//
// Sandbox: a profile under SHADETUBE_DATA_DIR, or a test that redirects the installer's locations, never touches the
// real values unless SHADETUBE_RUN_KEY names a test key (under HKCU; its StartupApproved stand-in is the subkey
// "StartupApproved"). Standalone (core only): tests/shortcuts and the installer tests compile it directly.
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace st::app::autostart {

inline constexpr wchar_t kFlag[] = L"--autostart";

bool allowed();                        // the registry may be touched (see above)
std::wstring runKey();                 // HKCU sub key of the Run value
std::wstring approvedKey();            // HKCU sub key of the StartupApproved value
std::wstring commandFor(const std::filesystem::path& exe);           // "\"<exe>\" --autostart"
// The exe of a Run command (quoted or not) and whether it carries --autostart. nullopt for an empty command.
struct Parsed {
    std::filesystem::path exe;
    bool autostartFlag = false;
};
std::optional<Parsed> parseCommand(std::wstring_view command);

std::optional<std::wstring> read();    // the current Run value (nullopt = none or not allowed)
bool write(const std::filesystem::path& exe);   // true when the value now holds commandFor(exe)
bool remove();                         // true when no value is left
bool disabledByWindows();              // StartupApproved says "disabled" (Task Manager / Settings › Startup)
void clearWindowsDisabled();           // forget that choice (the user just turned the option on here)

// Settings::startWithWindows applied: on -> write(exe) when it differs, off -> remove() when the value is ours.
// Returns what the registry holds afterwards (true = a Run value for `exe`).
bool sync(bool enabled, const std::filesystem::path& exe);

} // namespace st::app::autostart
