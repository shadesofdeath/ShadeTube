#pragma once
// Several Spotify accounts in the app. The store and the switch itself are spotify/Accounts + spotify/Session; this
// part decides which of the app's files belong to an account, where the active account's live, and gives the UI its
// actions: the account menu (the title bar's account chip, the command palette), the Settings rows, switching, adding
// and signing out with their toasts.
//
// Per account (accounts\<id>\ next to its cookie): the listening history (and its import), the blocklist, the new
// releases state, the smart lists cache, the Library's play recency. Per PC (the profile root): everything else -
// settings, themes, downloads and their sync rules (each Spotify rule remembers whose collection it is), local files,
// local playlists and likes, podcasts, radio. In memory, the Session swaps the library snapshot, Home and the friend
// activity reload; see docs/DEVELOPMENT.md ("Several Spotify accounts").
#include "gfx/Text.h"
#include "gfx/Types.h"
#include "ui/Anim.h"
#include "ui/Popups.h"
#include "ui/Widget.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace st::ui {
class Column;
}

namespace st::app::accounts {

// The names of the per-account files (a name also covers its ".old" / ".bak" / ".tmp" siblings).
const std::vector<std::wstring>& perAccountFiles();
// The active account's folder (the profile root while there is no account) and a file in it.
std::filesystem::path dataDir();
std::filesystem::path file(const wchar_t* name);

// The Spotify user the active account signs in as ("" logged out / not known yet).
std::string currentUser();

// Actions (UI thread). switchTo / signOut toast what happened; add() opens the login window for another account.
void switchTo(const std::string& accountId);
void add();
void signOut(const std::string& accountId);

// The account menu: the signed-in accounts (the active one checked; a click switches), "Hesap ekle…", "Hesapları
// yönet", "Çıkış yap" (logged out: "Spotify ile bağlan").
std::vector<ui::MenuItem> menuItems();
void openMenu(gfx::Point windowPos);

// Ayarlar › Spotify hesabı: the other signed-in accounts (avatar, name, Geç / Çıkış yap) and "Hesap ekle".
void buildSettingsRows(ui::Column* c, const std::function<void()>& rebuild);

// Dev: SHADETUBE_ACCOUNT_DEMO = `<action>@<ms>[=<arg>];…` after startup: `menu[=x,y]` (the account menu at window
// DIPs), `switch[=<id>]` (the next signed-in account, or that one), `signout[=<id>]` (the active one), `add` (the
// login window), `log` (the active account and its data folder).
void scheduleDemo();

// The title bar's source tag ("SPOTIFY · <NAME>" / "AÇIK KATALOG · MUSICBRAINZ") as a button that opens the account
// menu under it. Mono label in fg.tertiary like the tag it replaces; hover / focus show a chevron and a soft pill.
class AccountChip : public ui::Widget {
public:
    AccountChip();
    float naturalWidth();
    void paint(gfx::Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override;
    std::wstring tooltip() const override;

private:
    void sync();
    void open();
    std::wstring label_;
    gfx::Text text_;
    ui::Anim hover_;
    bool pressed_ = false;
};

} // namespace st::app::accounts
