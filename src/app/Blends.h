#pragma once
// Blend in the app (the pure part is app/Blend): the blends of this PC, their exchange over the listen-together
// transport, the consent dialog, the cards in "Senin için listeler" and the blend page (app/BlendPage.cpp).
//
// A blend is an invite link (shadetube://blend/<blend id | key | broker>, app/JamInvite) and what its members shared.
// Its topics (shadetube/blend/v1/<hex SHA-256(blend id)>/...): "p/<member id>" holds each member's taste profile as a
// retained message sealed with the blend key (AES-256-GCM, the topic as associated data: app/JamProtocol's envelope,
// type "taste"; "left" once that member left), so a member who opens the link later still finds it — nobody has to
// be online at the same time as anybody else. "x" carries "ask" (not retained): a member refreshed and asks those
// connected right now to share their current taste too (each of them is asked first).
//
// When this PC talks to the broker: right after creating (held up to 30 minutes until someone joins), joining or
// refreshing a blend; while its page is open; 20 s after startup and then every 6 hours for a few seconds per blend.
// Each visit re-publishes this PC's own profile as it was shared (never a newer one without asking) and reads the
// others'. Nothing is sent before the user saw it: creating, joining and refreshing open a dialog listing exactly the
// songs and artists that will go.
//
// Stored in %LOCALAPPDATA%\ShadeTube\blends.dat (DPAPI: it holds the keys), written on a worker. UI thread only.
#include "app/Blend.h"
#include "core/Async.h"
#include "gfx/Canvas.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace st::ui {
class Widget;
}

namespace st::app {

void initBlends();   // app startup (initFeatures): loads blends.dat, schedules the syncs

namespace blends {

// One blend as the UI shows it.
struct Info {
    std::string id;                    // route id (Route{RouteKind::SmartList, "blend:" + id}): 16 hex of the topic hash
    std::wstring title;                // "Ayşe + Berkay"
    blend::Mix mix;                    // blocklist not applied
    int me = -1;                       // this PC's index in mix.members (-1: not shared yet)
    bool waiting = false;              // nobody else has shared yet
    bool pending = false;              // joined, this PC hasn't shared its taste yet
    bool creator = false;
    std::string link;                  // the invite
    int64_t updatedAt = 0;             // unix seconds: the newest profile in it
    int64_t syncedAt = 0;              // the last exchange that reached a broker
    bool connected = false;            // talking to the broker right now
    std::vector<std::wstring> leftNames;   // members who left
};
bool loaded();                         // blends.dat has been read (until then a page shows its skeleton)
std::vector<Info> all();               // newest first
std::optional<Info> find(const std::string& id);
bool any();
// Changes (a profile arrived, a blend was added / left, the connection state). Dies with `owner`.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

void create();                                    // the consent dialog, then a new blend and its invite
void promptJoin();                                // "Blend davetiyle katıl" dialog
bool openInvite(const std::string& link);         // a pasted / clicked shadetube://blend/ link; false = not one
void share(const std::string& id);                // pending: the consent dialog, then this PC's taste goes
void refresh(const std::string& id);              // the consent dialog with today's taste, then it replaces the old one
void showShared(const std::string& id);           // what this PC shares in it (read only)
void copyInvite(const std::string& id);
void leave(const std::string& id);                // asks, then leaves (a "left" record) and forgets it
void watch(const std::string& id);                // its page is open: keep talking to the broker for a while

// "SEN", "AYŞE", "ORTAK": whose taste the row is (upper case, for the row's chip).
std::wstring tasteLabel(const Info& b, const blend::Row& row);
std::wstring subtitle(const Info& b);             // card line: "Blend · %87 uyum" / "Davet bekleniyor"
// The generated cover: a mosaic of the blend's covers above a band ("BLEND · %87", the names).
void drawCover(gfx::Canvas& c, const Info& b, const gfx::Rect& r, float radius);

// "Senin için listeler": the blend cards and the "Blend oluştur" card, added to `row`.
void addCards(ui::Widget* row);

struct PaletteCommand {
    std::string key;
    std::wstring title, subtitle;
    std::string icon;
    std::function<void()> run;
};
std::vector<PaletteCommand> paletteCommands();

} // namespace blends

class Page;
std::unique_ptr<Page> makeBlendPage(const std::string& id);   // app/BlendPage.cpp

} // namespace st::app
