#pragma once
// Composition root: creates services, windows and wires them together; owns the message loop.
#include "app/AppContext.h"
#include "app/DiscordRpc.h"
#include "app/MiniPlayer.h"
#include "app/Router.h"
#include "app/Scrobbler.h"
#include "app/SponsorBlock.h"
#include "app/Tray.h"
#include "app/WinShell.h"
#include "ui/Widget.h"

#include <memory>
#include <optional>
#include <string>

namespace st::ui {
class Window;
}

namespace st::app {

class Shell;
class Router;
class Smtc;

struct LaunchOptions {
    bool preview = false;          // --preview: open the shell without a Spotify session (UI checks)
    bool login = false;            // --login: open the Spotify WebView2 login window immediately (dev check)
    std::string previewPlay;       // --play "Artist - Title": resolve + play through the real pipeline
    std::string download;          // --download "Artist - Title": queue a real download (dev check)
    int screenshotAfterMs = 0;     // --screenshot <ms> <file.png>: capture the window and exit
    std::wstring screenshotPath;
    std::string route;             // --route search|library|settings|nowplaying
    bool mini = false;             // --mini: open the mini player right after startup (dev check; with --screenshot
                                   // the mini player window is captured)
    std::optional<ThemeMode> theme;  // --theme dark|light|system: overrides Settings::theme for this run (not saved)
    int themeSwitchAtMs = 0;         // --theme-at <ms> <mode>: live-switch to <mode> after <ms> (dev check, not saved)
    ThemeMode themeSwitchTo = ThemeMode::Dark;
    // --toast-at <ms> <text>: toast() after <ms> (dev check of the routing). "!" prefix = error; "~" prefix = first
    // hide every app window (tray state) and send the toast three times (tray-notification throttle).
    int toastAtMs = 0;
    std::wstring toastText;
    bool crashTest = false;        // --crash-test: crash right after startup (checks the crash report path)
    DWORD restartAfterPid = 0;     // --restart-after <pid>: App::restart() — wait for the old instance to exit
    // --command <name> (jump-list task) when no ShadeTube was running: carried out once the app is up.
    winshell::Command command = winshell::Command::None;
    // --autostart: Windows started us at sign-in (the Run value of "Windows ile başlat"). With Settings::startInTray the
    // main window stays hidden (tray icon only); a second autostart launch while one runs just exits.
    bool autostart = false;
    std::optional<std::wstring> palette;   // --palette [query]: open the command palette at startup (dev check)
};

// Windows' app mode (HKCU\...\Themes\Personalize AppsUseLightTheme; missing = light, the Windows default).
bool systemUsesLightTheme();
// Dark / light for a mode (System = systemUsesLightTheme()).
bool resolveLightTheme(ThemeMode mode);

// Registered window message a second launch posts to the running instance (main.cpp): restore the app from the
// tray / mini player. Both the main window and the mini player answer it.
inline constexpr wchar_t kActivateMessage[] = L"ShadeTube.Activate";

class App {
public:
    explicit App(LaunchOptions options);
    ~App();
    int run();

private:
    void showShell();
    void wirePlayer();
    void wireSession();
    void startLogin();
    void syncAccent(const catalog::Track& t);
    void showMatchPicker(const catalog::Track& t);
    bool handleKey(const ui::KeyEvent& e);
    void housekeeping();
    void captureScreenshot();
    void syncSmtc();             // player state -> Windows media flyout / lock screen
    void syncDiscord();          // player state -> Discord Rich Presence
    void sponsorTick();          // SponsorBlock: skip marked segments of the playing video
    bool sponsorActive() const;
    // Mini player / tray / window lifecycle.
    void openMini();             // shows the mini player and hides the main window
    void closeMini();            // "Büyüt" / close: back to the full window
    void showMain();             // shows / restores / focuses the main window
    void hideMainToTray();       // close with Settings::closeToTray
    void activate();             // tray click / second launch: the mini player if open, else the main window
    void postActivate();         // activate() outside the current window message
    void quit();                 // "Çıkış": really exits (saves window placements)
    void restart();              // quit + start a new instance (it waits for this process: --restart-after <pid>)
    void saveMainPlacement();
    void trimMemory();           // nobody sees the main window: hand back rebuildable GPU / working-set memory
    void syncTray();             // tooltip = "ShadeTube" + current track
    void syncThumbBar();         // player state -> taskbar thumbnail buttons
    void runCommand(winshell::Command c);   // thumbnail button / jump-list task / --command
    void postCommand(winshell::Command c);  // runCommand() outside the current window message
    void applyTheme();           // live theme switch: palette, accent, every window, tray menus
    void persist();              // flush everything to disk (exit / Windows session end)

    LaunchOptions options_;
    std::unique_ptr<youtube::MatchService> matcher_;
    std::unique_ptr<player::Player> player_;
    SponsorBlock sponsor_;               // UI thread; follows player_->currentMatch()
    std::unique_ptr<spotify::Session> session_;
    std::unique_ptr<ui::Window> window_;
    std::unique_ptr<Router> router_;
    std::unique_ptr<Smtc> smtc_;         // Windows media flyout / lock screen / media keys
    bool mediaHotkeys_ = false;          // RegisterHotKey fallback, only when SMTC is unavailable
    std::unique_ptr<Scrobbler> scrobbler_;
    std::unique_ptr<DiscordRpc> discord_;
    std::unique_ptr<MiniPlayer> mini_;   // only while the mini player is open
    std::unique_ptr<Tray> tray_;         // notification-area icon, for the whole app lifetime
    std::unique_ptr<winshell::ThumbBar> thumbBar_;   // Önceki / Oynat / Sonraki under the taskbar thumbnail
    UINT activateMsg_ = 0;               // kActivateMessage
    UINT commandMsg_ = 0;                // winshell::kCommandMessage (a jump-list task of a second launch)
    Shell* shell_ = nullptr;
    Route startRoute_{};                 // initial route; re-applied once the Spotify session logs in
    Lifetime life_;
    bool running_ = true;
    bool startupTrimmed_ = false;
    bool wasLoggedIn_ = false;
    double startedAt_ = 0;
    bool startHidden_ = false;           // --autostart into the tray: the main window was never shown yet
    bool everShown_ = false;             // the main window has been shown once (Window::show applies maximized)
};

} // namespace st::app
