#pragma once
// Spotify's social features, shown only with a Spotify session:
//   * Arkadaş etkinliği (app/FriendActivity.cpp): what the people the user follows play, a right-side panel toggled
//     from the title bar / command palette / a shortcut, refreshed every minute while it is on screen;
//   * Yeni çıkanlar (app/NewReleasesPage.cpp): new albums / singles / EPs of followed artists (model: app/NewReleases),
//     a page, a Home shelf, a sidebar badge and optional Windows notifications.
//
// Dev: SHADETUBE_SPOTIFY_DEMO=<folder> makes both available without a session and reads buddylist.json (the raw
// buddylist answer) and whatsnew.json (the raw queryWhatsNewFeed `data`) from that folder instead of Spotify.
#include "app/Components.h"
#include "app/Router.h"
#include "spotify/SpotifyApi.h"
#include "ui/Layout.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace st::app {

std::filesystem::path socialDemoDir();   // "" unless SHADETUBE_SPOTIFY_DEMO is set
bool socialAvailable();                  // a Spotify session (or the dev demo)

// ---- Friend activity ------------------------------------------------------------------------------------------------
// Right-side panel (the queue panel's slot: the two are never open together). Rows: avatar, name, "now" / how long
// ago, track · artist, and the context it plays from. A click on the track plays it in that context (playlist /
// album; else in its album), the artist opens the artist, the context its page; right click = the track menu.
class FriendPanel : public ui::Widget {
public:
    FriendPanel();
    void layout() override;
    void paint(Canvas& c) override;
    bool onWheel(float dy, float, const ui::MouseEvent&) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override;

    // Fetches now when the list is older than a minute (the panel was just opened / brought back on screen).
    void refreshIfStale();

private:
    enum class Part { None, Track, Artist, Context };
    struct Row {
        spotify::FriendActivity a;
        gfx::Text name, track, artist, context;
    };
    struct Hit {
        int row = -1;
        Part part = Part::None;
        bool operator==(const Hit&) const = default;
    };
    void tick();                 // housekeeping: the minute refresh while visible and the window is on screen
    void fetch();
    void setFriends(std::vector<spotify::FriendActivity> list);
    Hit hitAt(gfx::Point local) const;
    float listTop() const { return 20 + 36 + 12; }
    float contentHeight() const;
    void activate(const Hit& h, gfx::Point windowPos);

    ui::Button *refresh_, *close_;
    gfx::Text title_;
    std::vector<Row> rows_;
    struct Rects {
        Rect row, track, artist, context;
    };
    std::vector<Rects> rects_;   // panel-local, from the last paint
    ui::Anim scroll_;
    Hit hover_, pressed_;
    bool loaded_ = false, inFlight_ = false;
    std::wstring error_;         // last failure ("" = fine)
    double lastFetch_ = -1e12, retryAt_ = 0;   // ui::frame::realNow() ms
    Lifetime life_;
};

// ---- New releases ---------------------------------------------------------------------------------------------------
int newReleasesUnseenCount();   // sidebar badge (0 when unavailable)
// Home (logged in): a "Yeni çıkanlar" shelf of the last two weeks at the top, when there is anything.
void addNewReleasesShelf(ui::Column* c);
// Fired (UI thread) when the releases change; dies with the owner's Lifetime.
void subscribeNewReleases(Lifetime::Ref owner, std::function<void()> fn);
std::unique_ptr<Page> makeNewReleasesPage();

} // namespace st::app
