#pragma once
// Spotify podcasts in Podcastler: the shows the user follows on Spotify (Api::libraryShows) and the ones search finds,
// each resolved to its public RSS feed (podcast::Client::findFeed: Apple's directory by name + publisher; the verdict
// is kept in podcasts.json, podcast::SpotifyLink), and the import of the matched ones as subscriptions. ShadeTube
// never plays Spotify-hosted audio: a show without a public feed (a Spotify exclusive) is listed as unavailable, never
// left out. Nothing is subscribed without the user's action. UI thread only; the network runs on workers.
#include "app/Components.h"
#include "catalog/Models.h"
#include "ui/Controls.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::app {

// The directory's store for charts, search and the Spotify lookups: Windows' home location, else the app region.
std::string podcastDirectoryCountry();

namespace spotifyshows {

// ---- The shows the user follows --------------------------------------------------------------------------------------
enum class ListState { NoSession, Loading, Ready, Failed };
ListState listState();
const std::vector<catalog::PodcastShow>& followed();   // most recently followed first
// Loads the list when there is none yet, it is 10 minutes old, the account changed or `force`. No session: cleared.
void ensureFollowed(bool force = false);

// Shows seen this session (followed, search results, opened): known(uri) finds them again.
void remember(const catalog::PodcastShow& show);
const catalog::PodcastShow* known(const std::string& uri);

// ---- Feed lookups ------------------------------------------------------------------------------------------------------
enum class Lookup { Unknown, Pending, Matched, NoFeed, Failed };
// Matched / NoFeed: a fresh verdict in the store; Pending: queued or being looked up; Failed: the directory could not be
// asked (retried after two minutes or on demand); Unknown: not looked up (or a week-old "no feed").
Lookup lookup(const std::string& uri);
// Queues the lookup unless the verdict is fresh (`force`: look again anyway). `urgent`: ahead of the others (a page
// waits for it). One at a time, spaced as podcast::Client::findFeed requires.
void resolve(const catalog::PodcastShow& show, bool urgent, bool force = false);
void resolveAll(const std::vector<catalog::PodcastShow>& shows);
// The user says the matched feed is another show: no feed from now on (a later lookup never picks that one again).
void reject(const std::string& uri);

// The list loaded or a lookup finished / failed (store changes also reach podcast::store()'s own listeners). Called on
// the UI thread while `owner` lives.
void subscribe(std::weak_ptr<const void> owner, std::function<void()> fn);

// ---- Import --------------------------------------------------------------------------------------------------------------
// Subscribes to the matched shows among `uris` that aren't subscribed yet: each feed is fetched first (one at a time),
// then subscribed like the show page's button does. Toasts the outcome.
void importShows(const std::vector<std::string>& uris);
bool importing();
// Matched, not subscribed: what the import offers.
bool importable(const std::string& uri);
// The matched feed is already a subscription.
bool subscribed(const std::string& uri);

// "Abonesin" / the publisher / "Yalnızca Spotify'da" / "Aranıyor…": a card's second line.
std::wstring cardSubtitle(const catalog::PodcastShow& show);
// https://open.spotify.com/show/<id>
std::wstring webUrl(const std::string& uri);

// ---- The import preview's row ------------------------------------------------------------------------------------------
// Cover, the show as Spotify names it, and what it resolved to: the directory entry with a toggle (selected for the
// import), "Abonesin", "no public feed" or the lookup in progress. A click opens the show; Enter / Space flip the
// toggle of an importable show, else open it.
class ShowRow : public ui::Widget {
public:
    explicit ShowRow(catalog::PodcastShow show);
    std::function<void(bool)> onCheck;
    std::function<void()> onOpen;
    const catalog::PodcastShow& show() const { return show_; }
    void setChecked(bool on);
    bool checked() const;
    void refresh();   // re-reads the lookup and subscription state (call after store / lookup changes)

    float preferredHeight(float) override { return 72; }
    void layout() override;
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override;

private:
    catalog::PodcastShow show_;
    ui::Toggle* toggle_ = nullptr;
    gfx::Text title_, publisher_, status_, detail_;
    std::string statusIcon_;
    Lookup state_ = Lookup::Unknown;
    bool pressed_ = false;
    ui::Anim hover_;
};

} // namespace spotifyshows
} // namespace st::app
