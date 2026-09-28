#pragma once
// Per-user install / uninstall without a separate setup exe: the app installs itself (no admin rights), the way
// Spotube's per-user Windows installer lays things out.
//   install    copy the running exe to %LOCALAPPDATA%\Programs\ShadeTube\ShadeTube.exe, a Start menu shortcut
//              (FOLDERID_Programs\ShadeTube.lnk) and HKCU\...\Uninstall\ShadeTube, so Windows Ayarlar › Uygulamalar ›
//              Yüklü uygulamalar lists it and runs `"<exe>" --uninstall` (QuietUninstallString adds --quiet).
//   uninstall  shortcut + uninstall key go at once; the install folder (and, when asked, the user data) is deleted
//              by a detached, windowless cmd.exe retry loop once this process has exited.
// Tests / dev override the locations with SHADETUBE_INSTALL_DIR, SHADETUBE_SHORTCUT_DIR and SHADETUBE_UNINSTALL_KEY
// ("HKCU\Software\ShadeTubeTest\Uninstall\ShadeTube"; always under HKCU). They are read only when set.
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::installer {

struct Locations {
    std::filesystem::path dir;        // %LOCALAPPDATA%\Programs\ShadeTube
    std::filesystem::path exe;        // dir\ShadeTube.exe
    std::filesystem::path shortcut;   // %APPDATA%\Microsoft\Windows\Start Menu\Programs\ShadeTube.lnk
    std::wstring uninstallKey;        // under HKEY_CURRENT_USER
};
Locations locations();

// The uninstall key's values (exists = false when ShadeTube is not registered).
struct Registration {
    bool exists = false;
    std::wstring displayName, displayVersion, publisher, displayIcon, installLocation, uninstallString, quietUninstallString;
    uint32_t estimatedSizeKb = 0, noModify = 0, noRepair = 0;
};
Registration readRegistration();

std::optional<std::filesystem::path> installedExe();   // InstallLocation\ShadeTube.exe of the uninstall key, if it exists
bool runningInstalled();                               // the running exe is the installed one

// Copies `sourceExe` into the install folder (replacing an older install), writes the uninstall key and creates the
// shortcut. Throws std::runtime_error with a message in the UI language (UTF-8). Initializes COM on the calling thread
// when needed.
void install(const std::filesystem::path& sourceExe);
void writeRegistration(const std::filesystem::path& exe);   // (re)writes every value for `exe`
void refreshRegistration();   // installed run: DisplayVersion / EstimatedSize follow the exe (after an update)
std::filesystem::path shortcutTarget(const std::filesystem::path& lnk);   // IShellLink target; empty if unreadable

void unregister();   // shortcut + uninstall key (idempotent)
// unregister(), then deletes the install folder (only ShadeTube's own files when other files live there) and, with
// `removeUserData`, the data folder (paths::appData()). Whatever this process still uses goes once it has exited.
void uninstall(bool removeUserData);
// Deletes files / folders from a detached, windowless cmd.exe that retries for about a minute, i.e. until this
// process has exited. The paths reach cmd.exe as environment variables (delayed expansion), never as parsed text.
// Returns false when the helper could not be started.
bool deleteAfterExit(const std::vector<std::filesystem::path>& paths);

// Starts `"exe" args` once this process has exited: after main() returned, so the single-instance mutex is free
// (install -> "--installed", update -> "--updated").
void launchAfterExit(const std::filesystem::path& exe, std::wstring args);
bool hasArg(std::wstring_view flag);   // the flag is on this process's command line (case-insensitive)

// Another ShadeTube process (from any folder) has a window: the shared data folder must not be deleted under it.
bool otherInstanceRunning();

// `ShadeTube.exe --uninstall [--quiet]`, what Windows runs for "Kaldır". main.cpp calls it before the single-instance
// check (uninstallRequested()): confirm dialog with "Ayarları ... de sil" (--quiet: none, user data kept), asks a
// ShadeTube running from the install folder to quit, uninstalls, reports. With data removal, copies running from other
// folders are asked to quit too; if one stays, the data is kept (and the report says so).
// Returns the process exit code (1602 = cancelled by the user).
// Test hooks: SHADETUBE_INSTANCE_CLASS / SHADETUBE_INSTANCE_MUTEX replace the window class / mutex that identify a
// running ShadeTube (read only when set).
bool uninstallRequested();
int runUninstallCommand();

} // namespace st::app::installer
