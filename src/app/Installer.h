#pragma once
// Per-user install / uninstall (no admin rights), the way Spotube's per-user Windows installer lays things out. Shared
// by the app (st_install: the installed copy's registration refresh and "Uygulamayı kaldır", `--uninstall`) and the
// setup (src/setup, ShadeTubeSetup.exe), which carries the app and installs it with install().
//   install    copy the exe to %LOCALAPPDATA%\Programs\ShadeTube\ShadeTube.exe (or another folder), a Start menu
//              shortcut (FOLDERID_Programs\ShadeTube.lnk, carrying the app's AppUserModelID), optionally one on the
//              desktop, and HKCU\...\Uninstall\ShadeTube, so Windows Ayarlar › Uygulamalar › Yüklü uygulamalar lists it
//              and runs `"<exe>" --uninstall` (QuietUninstallString adds --quiet).
//   uninstall  shortcuts, uninstall key and app identity (AppUserModelId key, <data>\shell, jump list) go at once; the
//              install folder (and, when asked, the user data) is deleted by a detached, windowless cmd.exe retry
//              loop once this process has exited.
// Tests / dev override the locations with SHADETUBE_INSTALL_DIR, SHADETUBE_SHORTCUT_DIR, SHADETUBE_DESKTOP_DIR and
// SHADETUBE_UNINSTALL_KEY ("HKCU\Software\ShadeTubeTest\Uninstall\ShadeTube"; always under HKCU). They are read only
// when set. SHADETUBE_SETUP_SANDBOX=<folder> sets all of them at once (applySandbox()).
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
    std::filesystem::path desktopShortcut;   // FOLDERID_Desktop\ShadeTube.lnk (only the setup creates it)
    std::wstring uninstallKey;        // under HKEY_CURRENT_USER
};
Locations locations();

// SHADETUBE_SETUP_SANDBOX=<folder>: every location the installer, the setup and an app started from them touch is
// redirected into that folder or under HKCU\Software\ShadeTubeSandbox\<id> (id: a hash of the folder), by setting the
// variables above plus SHADETUBE_RUN_KEY, SHADETUBE_DATA_DIR, SHADETUBE_INSTANCE_CLASS and SHADETUBE_INSTANCE_MUTEX
// (each only when it is not set already) in this process, so child processes inherit them. Start menu, desktop,
// "Installed apps", the Run value, the app identity and running ShadeTubes of the user are never touched. Returns the
// sandbox's registry root ("Software\ShadeTubeSandbox\<id>"), empty when the variable is not set.
std::wstring applySandbox();

// The uninstall key's values (exists = false when ShadeTube is not registered).
struct Registration {
    bool exists = false;
    std::wstring displayName, displayVersion, publisher, displayIcon, installLocation, uninstallString, quietUninstallString;
    uint32_t estimatedSizeKb = 0, noModify = 0, noRepair = 0;
};
Registration readRegistration();

std::optional<std::filesystem::path> installedExe();   // InstallLocation\ShadeTube.exe of the uninstall key, if it exists
bool runningInstalled();                               // the running exe is the installed one

struct InstallOptions {
    std::filesystem::path dir;             // install folder; empty = locations().dir
    bool startMenuShortcut = true;
    bool desktopShortcut = false;
    // Files copied next to the exe under their own names (the licenses the setup carries); see isOwnFile().
    std::vector<std::filesystem::path> extraFiles;
};
// Copies `sourceExe` into the install folder (replacing an older install: a copy next to it, then one rename; an exe
// still running in another session is renamed away), copies the extra files, writes the uninstall key and creates the
// shortcuts asked for (one of ours that is not asked for any more is removed). Throws std::runtime_error with a
// message in the UI language (UTF-8). Initializes COM on the calling thread when needed.
void install(const std::filesystem::path& sourceExe, const InstallOptions& options = {});
void writeRegistration(const std::filesystem::path& exe);   // (re)writes every value for `exe`
void refreshRegistration();   // installed run: DisplayVersion / EstimatedSize follow the exe (after an update)
std::filesystem::path shortcutTarget(const std::filesystem::path& lnk);   // IShellLink target; empty if unreadable
// `lnk` exists and starts `exe` (or any ShadeTube.exe in `exe`'s folder).
bool shortcutPointsAt(const std::filesystem::path& lnk, const std::filesystem::path& exe);
// Names ShadeTube itself puts in its folder: the exe and its temp copy, the licenses, the portable package's readme
// and the exact names the updater creates. Anything else in the folder counts as the user's and is never deleted.
bool isOwnFile(const std::filesystem::path& name);

// "Windows ile başlat" from outside the app (the setup): the app's own setting (settings.json "startWithWindows",
// patched in place: every other value stays as it is) and the Run value for `exe`. The app re-syncs the Run value
// from that setting at every start, so both have to agree. Returns false when the settings file could not be written.
bool setStartWithWindows(bool on, const std::filesystem::path& exe);
bool startWithWindowsSetting();   // settings.json says so (false without a file)

// Shortcuts + uninstall key + app identity (idempotent). The identity is shared by every copy: it stays while another
// ShadeTube runs (a window of instanceClass() in another process), which registers it again at its next start anyway.
// The desktop shortcut goes only when it starts the installed copy (a user's own one to a portable copy stays).
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
// Starts `"exe" args` now, detached from this process (and its job object when allowed), in the exe's folder. From an
// elevated process (a setup started "as administrator") the app is started through Explorer instead, so it runs
// with the user's normal rights. False when it could not be started.
bool launch(const std::filesystem::path& exe, const std::wstring& args);
bool hasArg(std::wstring_view flag);   // the flag is on this process's command line (case-insensitive)

// Another ShadeTube process (from any folder) has a window: the shared data folder must not be deleted under it.
bool otherInstanceRunning();

// A running ShadeTube: a process (not this one) with a window of instanceClass(), its UI thread and its exe.
struct Instance {
    unsigned long pid = 0, tid = 0;
    std::filesystem::path image;
};
std::vector<Instance> runningInstances();
// Asks them to quit (WM_QUIT to the UI thread: App::run ends and saves everything on the way out, like "Çıkış") and
// waits up to `timeoutMs` for each. False when one of them is still running.
bool closeInstances(const std::vector<Instance>& instances, unsigned long timeoutMs = 30000);
// The single-instance mutex is held (a ShadeTube that may have no window yet). Not meaningful inside the app.
bool instanceMutexHeld();

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
