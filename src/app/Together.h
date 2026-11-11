#pragma once
// "Birlikte dinle" (listen together, Spotify calls it Jam): friends hear the same song at the same moment, with no
// account of any kind and no server of ours. The core is standalone (app/Jam*: MQTT over WebSocket to a public broker,
// end-to-end encryption, the invite link, the state model, the guest's reconciliation and clock); this is the app side:
// the session on the UI thread, wired to the player, the side panel (app/TogetherPanel.cpp), the paste paths, the
// settings rows and the command palette.
//
// Host: starts a session (a room id + key, the invite link), connects to the first broker that answers and publishes a
// retained, sealed SessionState on every change of the player (coalesced) and every 30 s: the current item and the next
// 50 (title, artists, cover, length, the exact YouTube video the host plays), position + the host's clock, playing,
// guest permissions, members, bans. Guests' requests ("add" always; play / pause / skip / seek / jump only with
// "Misafirler çalmayı kontrol edebilir") come in on the guest topic; added songs go into the host's queue after the
// songs guests added before, marked with the guest's name.
// Guest: its own queue is saved (a copy of session.json + shuffle / repeat / speed in jam.json) and comes back when it
// leaves, is removed or the host ends the session (also after a crash: the next start restores it). It follows the
// host with jam::decide() four times a second: the same video (MatchService::pinForSession), the host's position on
// the estimated host clock (pings), re-seeking past 1 s of drift. The player's commands go through
// Player::commandFilter: songs a guest plays or queues become "add" requests, transport becomes requests or is
// refused (with a toast), queue edits are refused; volume stays local.
// Podcast episodes and radio stations are not shared (guests pause and the panel says why); items the host plays from
// its disk are looked up by the guest (the host's video when it knows one, else by title / artist / length).
//
// Presence: hello / heartbeats every 10 s / last will ("bye"); 35 s of silence = offline. At most 10 people. Removing
// someone bans the device (the host ignores it, the state lists it, guests drop it); "Yeni davet bağlantısı" moves the
// session to a new room + key and hands the new link to each remaining online guest sealed for that guest alone (ECDH),
// so a removed person can't follow or rejoin.
// Brokers: the invite's broker first, then Settings::jamBroker, then the built-in list (jam::brokerOrder). A guest that
// hears nothing from the host for 35 s, or finds no session within 15 s, tries the next broker (the host fails over in
// the same order and re-publishes there).
//
// Dev: SHADETUBE_JAM_HOST=1 starts a session at startup and SHADETUBE_JAM_INVITE_FILE=<path> receives its invite link;
// SHADETUBE_JAM_JOIN=<invite link> joins one at startup (two sandbox profiles, see docs). Keys and links are never
// logged (logs carry jam::roomTag()).
#include "catalog/Models.h"
#include "core/Async.h"
#include "gfx/Canvas.h"
#include "gfx/Text.h"
#include "ui/Widget.h"

#include <functional>
#include <string>
#include <vector>

namespace st::ui {
class Column;
class ScrollView;
class Button;
} // namespace st::ui

namespace st::app {

// App startup (through initFeatures()): the device id, the player hooks, a guest queue left by a crash, the shadetube:
// URL protocol (real profile only) and the dev variables.
void initTogether();

namespace together {

enum class Role { None, Host, Guest };
enum class Link { Off, Connecting, Connected, Reconnecting };

struct MemberView {
    std::string device;
    std::wstring name;
    bool online = true;
    bool host = false;
    bool me = false;
};

// What the panel and the player bar show (UI thread).
struct View {
    Role role = Role::None;
    bool joining = false;          // guest: no session state yet
    Link link = Link::Off;
    std::string broker;            // host name of the broker in use
    std::wstring hostName;
    std::string invite;            // host: the current invite link
    std::vector<MemberView> members;
    bool guestsControl = false;
    int online = 0;                // members online, the host included
};
View view();
Role role();
bool active();                     // host or guest
bool guest();
bool guestCanControl();            // a guest and the host allows control
// Guest: how it follows ("Eşit · ±0,12 sn", "Eşitleniyor…", why it holds). "" otherwise.
std::wstring syncText();
bool syncOk();                     // guest: lined up (for the dot's color)
// Fired (UI thread) when the session changes (role, members, link, permissions); dies with `owner`.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

// The name others see: Settings::jamName, else the Spotify display name, else the Windows user's first name.
std::string defaultName();   // without Settings::jamName
std::string displayName();

void startHosting();
void promptJoin();                          // "Oturuma katıl" dialog
bool join(const std::string& inviteText);   // false: not a valid invite link (nothing happens)
void leave();                               // guest: leave; host: end the session (asks first)
void endSession();                          // host, no question
void copyInvite();
void rotateInvite();                        // host: "Yeni davet bağlantısı"
void removeMember(const std::string& device);
void setGuestsControl(bool on);
void moveToNextBroker();                    // host: "Başka sunucuya taşı"

// Queue panel: the guest who added `t` ("" = nobody).
std::wstring addedBy(const catalog::Track& t);
// The "+ AYŞE" chip (accent tint, like smart shuffle's badge), left edge at x, centered on cy; returns its width.
float drawAddedBadge(gfx::Canvas& c, float x, float cy, const std::wstring& name);
float addedBadgeWidth(const std::wstring& name);

// Player bar: "Birlikte dinleniyor · <host>" / "… · 3 kişi" ("" outside a session).
std::wstring barLabel();
// Player bar: why a guest's transport is disabled ("" when it isn't).
std::wstring lockedReason();

// Command palette entries (what can be done now).
struct PaletteCommand {
    std::string key;
    std::wstring title;
    std::string icon;
    std::function<void()> run;
};
std::vector<PaletteCommand> paletteCommands();

// A shadetube: link handed over by a second launch (main.cpp, WM_COPYDATA): opens it like a pasted link.
void openForwardedLink(const std::wstring& text);

} // namespace together

// "Birlikte dinle" side panel (the queue's slot; the queue, the friend activity and this one are never open together).
class JamPanel : public ui::Widget {
public:
    JamPanel();
    void layout() override;
    void paint(ui::Canvas& c) override;
    bool onWheel(float dy, float dx, const ui::MouseEvent& e) override;
    void rebuild();

private:
    ui::Button* close_;
    ui::ScrollView* scroll_;
    gfx::Text title_;
    together::Role builtFor_ = together::Role::None;
    Lifetime life_;
};


} // namespace st::app
