#pragma once
// System tray (notification area) icon: app icon + "ShadeTube / <track>" tooltip, left click restores the app,
// right click opens a native Turkish menu (Oynat/Duraklat, Sonraki, Önceki, ShadeTube'u göster, Çıkış).
//
// The icon lives for the whole app lifetime (not only while the window is hidden): with the mini player open
// the main window is hidden and the mini player is a tool window, so the tray icon is the app's only taskbar
// presence; it also makes "close to tray" discoverable before the user turns it on.
//
// Owns a hidden top-level window for the callbacks (a message-only window would miss the "TaskbarCreated"
// broadcast that tells us to re-add the icon after Explorer restarts). UI thread only.
#include <functional>
#include <string>

#include <windows.h>

namespace st::app {

class Tray {
public:
    enum class Command { TogglePlay, Next, Previous, Show, Quit };
    struct MenuState {
        bool playing = false;    // "Duraklat" instead of "Oynat"
        bool hasTrack = false;   // transport items enabled
    };

    explicit Tray(bool darkMenus);   // native menus follow the app theme (process-wide uxtheme app mode)
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool added() const { return added_; }
    // Tooltip text (the shell caps it at 127 characters; longer text is cut with an ellipsis).
    void setTooltip(const std::wstring& tip);
    // Notification balloon (Windows 10/11 shows it as a toast from the app). `error` = error icon.
    void balloon(const std::wstring& title, const std::wstring& text, bool error = false);
    // Live theme switch: the tray menu (and every native popup menu of the process) follows the new mode.
    void setDarkMenus(bool dark);

    std::function<void()> onActivate;              // left click / keyboard select
    std::function<MenuState()> menuState;          // queried right before the menu opens
    std::function<void(Command)> onCommand;

private:
    static LRESULT CALLBACK wndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp);
    bool add();                                    // NIM_ADD + NIM_SETVERSION (logs the result)
    void loadIcon();
    void showMenu(POINT anchor);
    void run(const std::function<void()>& fn) const;   // exceptions never escape into the shell

    HWND hwnd_ = nullptr;
    HICON icon_ = nullptr;
    UINT taskbarCreated_ = 0;
    std::wstring tip_;
    bool added_ = false;
};

} // namespace st::app
