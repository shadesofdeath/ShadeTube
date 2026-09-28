#pragma once
// Per-user install / uninstall without a separate setup exe: the app installs itself (no admin rights), the way
// Spotube's per-user Windows installer lays things out.
//   install    copy the running exe to %LOCALAPPDATA%\Programs\ShadeTube\ShadeTube.exe, a Start menu shortcut
//              (FOLDERID_Programs\ShadeTube.lnk, carrying the app's AppUserModelID) and HKCU\...\Uninstall\ShadeTube,
//              so Windows Ayarlar › Uygulamalar › Yüklü uygulamalar lists it and runs `"<exe>" --uninstall`
//              (QuietUninstallString adds --quiet).
//   uninstall  shortcut, uninstall key and app identity (AppUserModelId key, <data>\shell, jump list) go at once; the
//              install folder (and, when asked, the user data) is deleted by a detached, windowless cmd.exe retry
//              loop once this process has exited.
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

// Shortcut + uninstall key + app identity (idempotent). The identity is shared by every copy: it stays while another
// ShadeTube runs (a window of instanceClass() in another process), which registers it again at its next start anyway.
void unregister();
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

// What identifies a running ShadeTube: the window class of its windows ("ShadeTube.Window") and main.cpp's
// single-instance mutex. Test hooks: SHADETUBE_INSTANCE_CLASS / SHADETUBE_INSTANCE_MUTEX replace them (read only when
// set), so a test instance neither finds nor is found by the ShadeTube the user runs.
std::wstring instanceClass();
std::wstring instanceMutex();

// `ShadeTube.exe --uninstall [--quiet]`, what Windows runs for "Kaldır". main.cpp calls it before the single-instance
// check (uninstallRequested()): confirm dialog with "Ayarları ... de sil" (--quiet: none, user data kept), asks a
// ShadeTube running from the install folder to quit, uninstalls, reports. With data removal, copies running from other
// folders are asked to quit too; if one stays, the data is kept (and the report says so).
// Returns the process exit code (1602 = cancelled by the user).
// Test hooks: SHADETUBE_INSTANCE_CLASS / SHADETUBE_INSTANCE_MUTEX replace the window class / mutex that identify a
// running ShadeTube (read only when set).
bool uninstallRequested();
int runUninstallCommand();

// ---- App identity (used by app/WinShell) ------------------------------------------------------------------------
// The AppUserModelID (AUMID) of the process, its windows, the Start menu shortcut, the jump list and the media session.
// Windows groups the taskbar button by it, and the media flyout names an unpackaged app after the Start menu shortcut
// that carries the same AUMID (the shell's Apps folder). SHADETUBE_AUMID replaces it (tests; read only when set; a
// malformed override never falls back to the real id).
inline constexpr wchar_t kAppUserModelId[] = L"shadesofdeath.ShadeTube";
std::wstring appUserModelId();
bool appUserModelIdOverridden();
// A test AUMID without SHADETUBE_SHORTCUT_DIR: the Start menu shortcut is the user's real one, so it is neither created
// nor given the test id (ensureStartShortcut does nothing, install() writes it without an AUMID).
bool leaveRealShortcut();
// HKCU\Software\Classes\AppUserModelId\<AUMID> (DisplayName + IconUri): how Windows names and draws the notifications
// of an app without a package.
std::wstring appIdKey();
std::filesystem::path shellDir();   // paths::appData()\shell: files the shell reads (the IconUri PNG, jump-list icons)
struct AppIdentity {
    bool exists = false;
    std::wstring displayName, iconUri;
};
AppIdentity readAppIdentity();
// Writes DisplayName "ShadeTube" and IconUri = `iconPng` where they differ. True when a value was written.
bool registerAppIdentity(const std::filesystem::path& iconPng);
// The key, shellDir() and the jump list (idempotent). Part of unregister(), except in a test run that overrides the
// install locations without a test AUMID (it never touches the real identity) and while another ShadeTube runs.
void unregisterAppIdentity();

std::wstring shortcutAppId(const std::filesystem::path& lnk);   // System.AppUserModel.ID of a shortcut ("" if none)
// The Start menu shortcut the media flyout needs. A registered install owns it (only the AUMID is added to an older
// shortcut of the installed exe). A portable copy gets one to itself, once: a shortcut the user deleted stays deleted
// (a marker in shellDir() remembers it was created); one whose exe is gone is pointed at `exe`, one to another existing
// copy only gets the AUMID. Needs COM on the calling thread (initialized when missing).
enum class ShortcutAction { None, Stamped, Created, Retargeted };
ShortcutAction ensureStartShortcut(const std::filesystem::path& exe);

} // namespace st::app::installer
