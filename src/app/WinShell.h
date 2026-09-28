#pragma once
// Windows shell integration: the app's identity (AppUserModelID), the taskbar thumbnail toolbar (Önceki / Oynat or
// Duraklat / Sonraki), the jump list tasks and the `--command` launches they make.
//
// Identity. Windows groups the taskbar button, keys the jump list and attributes the media session by the
// AppUserModelID (AUMID, installer::appUserModelId()). An unpackaged app gets its name and icon in the media flyout
// from the Start menu shortcut carrying the same AUMID (the shell's Apps folder); without one the flyout says
// "Unknown app". So:
//   - main.cpp sets the process AUMID before any window exists (setProcessAppId: every window inherits it) and App
//     also puts it on the main window and the mini player with the relaunch properties used when a button is pinned
//     (setWindowIdentity);
//   - the Start menu shortcut carries the AUMID (installer: install() / ensureStartShortcut() for a portable copy);
//   - HKCU\Software\Classes\AppUserModelId\<AUMID> names the app's notifications (DisplayName + IconUri: the app
//     icon as a PNG written to <data>\shell only when it changed).
// A sandbox profile (SHADETUBE_DATA_DIR) keeps Windows' default identity and leaves the machine-wide parts (key,
// shortcut, jump list) alone, unless SHADETUBE_AUMID gives a test id: dev runs never group with, or rewrite the shell
// entries of, the ShadeTube the user runs. Even then the Start menu shortcut is only touched in a test folder
// (SHADETUBE_SHORTCUT_DIR): the real one belongs to the real app (installer::leaveRealShortcut).
//
// Commands. A jump-list task runs `ShadeTube.exe --command <name>`. main.cpp forwards it to a running instance as the
// registered kCommandMessage (wParam = Command) and exits without UI (it waits a few seconds for the window of an
// instance that is still starting); with no instance running, or when the running one exits meanwhile, the new one
// carries it out after startup (LaunchOptions::command).
//
// Standalone module (tests/winshell compiles it): core/*, app/Installer and the Windows SDK only. UI thread unless noted.
#include <windows.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace st::app {

// App startup (called once through initFeatures()): identity registration and the jump list, on a worker.
void initWinShell();

namespace winshell {

enum class Command : int { None = 0, PlayPause = 1, Next = 2, Previous = 3, Mini = 4 };
Command parseCommand(std::wstring_view name);         // "play-pause" / "next" / "previous" / "mini"; None otherwise
const wchar_t* commandName(Command c);                // the inverse ("" for None)
Command commandFromArgs(const wchar_t* commandLine);  // `--command <name>` on a command line (GetCommandLineW())

// Registered window message a `--command` launch posts to the running instance: wParam = Command.
inline constexpr wchar_t kCommandMessage[] = L"ShadeTube.Command";
// A second launch: posts `c` to the running ShadeTube (its main window "ShadeTube" of `windowClass`, else any window of
// that class, hidden ones included). The mini player command hands over the foreground right. False: none found.
bool forwardCommand(Command c, const wchar_t* windowClass = L"ShadeTube.Window");

// ---- Identity ----
// All three do nothing unless registrationAllowed().
bool setProcessAppId();                // SetCurrentProcessExplicitAppUserModelID, before any window; true when set
void setWindowIdentity(HWND hwnd);     // AUMID + RelaunchCommand / RelaunchDisplayNameResource / RelaunchIconResource
void clearWindowIdentity(HWND hwnd);   // before the window is destroyed (the shell asks for VT_EMPTY)
bool registrationAllowed();            // not a sandbox profile, or a test AUMID
std::filesystem::path currentExe();
// The exe's icon (resource 1) at 256 px as PNG bytes (its PNG image as is; any other image encoded with WIC).
std::vector<uint8_t> appIconPng();
// Registry key + IconUri PNG + Start menu shortcut. Any thread (initializes COM when needed); logs what it changed.
void registerIdentity(const std::filesystem::path& exe);
// Writes `bytes` to `path` unless the file already holds exactly them. True when written.
bool writeIfChanged(const std::filesystem::path& path, const std::vector<uint8_t>& bytes);

// ---- Glyph icons (the SVG icons of assets/icons, as the app draws them) ----
// Windows' own light / dark mode for the taskbar, Start and jump lists (HKCU ...\Personalize SystemUsesLightTheme).
bool systemUsesLightTheme();
uint32_t glyphColor(bool lightSurface);   // 0xRRGGBB: near-black on light surfaces, white on dark ones
// Rasterizes icons/<16|20|24>/<name>.svg at px x px: top-down 0xAARRGGBB, straight alpha, tinted `rgb`. Empty on
// failure. Any thread with COM initialized (WIC bitmap + Direct2D software target).
std::vector<uint32_t> renderGlyph(std::string_view name, int px, uint32_t rgb);
HICON makeIcon(const std::vector<uint32_t>& argb, int px);   // nullptr on failure; DestroyIcon when done
struct IcoImage {
    int px = 0;
    std::vector<uint32_t> argb;
};
std::vector<uint8_t> icoFile(const std::vector<IcoImage>& images);   // .ico bytes (32-bit BMP entries)

// ---- Jump list ----
struct JumpTask {
    Command command;
    std::wstring title;   // tr()
    const char* glyph;    // icon name in assets/icons
};
std::vector<JumpTask> jumpTasks();   // Oynat / Duraklat, Sonraki, Önceki, (separator) Mini oynatıcı
// Writes the task icons for the theme to installer::shellDir() and commits the list for installer::appUserModelId().
// Any thread (initializes COM when needed).
bool buildJumpList(const std::filesystem::path& exe, bool lightTheme);
// Windows' theme changed (WM_SETTINGCHANGE "ImmersiveColorSet"): rebuilds the list with matching icons on a worker,
// only when the mode really changed and registration is allowed.
void refreshJumpList();

// ---- Taskbar thumbnail toolbar ----
// Önceki / Oynat or Duraklat / Sonraki under the main window's taskbar thumbnail (ITaskbarList3). The buttons exist
// only while the window has a taskbar button: they are (re)added on every "TaskbarButtonCreated" (first show, shown
// again from the tray / mini player, Explorer restarted). Icons follow the window's DPI and Windows' theme.
class ThumbBar {
public:
    struct State {
        bool hasTrack = false;   // anything to play: otherwise every button is disabled
        bool playing = false;    // Duraklat instead of Oynat
        bool canNext = false;
        bool operator==(const State&) const = default;
    };
    // What a state shows (pure; tests). Order: previous, play/pause, next.
    struct Button {
        UINT id;
        Command command;
        const char* glyph;
        const wchar_t* tooltip;   // tr()
        bool enabled;
    };
    static std::array<Button, 3> describe(const State& s);

    explicit ThumbBar(HWND hwnd);
    ~ThumbBar();
    ThumbBar(const ThumbBar&) = delete;
    ThumbBar& operator=(const ThumbBar&) = delete;

    // Every message of the window. True when it was only ours (TaskbarButtonCreated, a thumbnail button click);
    // DPI and theme changes are seen but left to the window (false).
    bool handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    void setState(const State& s);
    bool added() const { return added_; }

    std::function<void(Command)> onCommand;   // a button click, inside the window procedure

private:
    void add();
    void update();
    bool renderIcons(UINT dpi);   // for `dpi` and Windows' theme; false when unchanged
    std::array<THUMBBUTTON, 3> buttons() const;

    HWND hwnd_;
    UINT buttonCreatedMsg_ = 0;
    Microsoft::WRL::ComPtr<ITaskbarList3> taskbar_;
    State state_{};
    std::array<HICON, 4> icons_{};   // prev, play, pause, next
    int iconPx_ = 0;
    bool iconLight_ = false;
    bool added_ = false;
    bool updateFailureLogged_ = false;
};

} // namespace winshell
} // namespace st::app
