#pragma once
// Application chrome: title bar, sidebar, player bar, queue panel, page host and the root layout.
#include "app/AppContext.h"
#include "app/Components.h"
#include "app/DragDrop.h"
#include "app/Router.h"
#include "ui/Controls.h"
#include "ui/Layout.h"

#include <memory>

namespace st::app {

class NowPlayingView;
class ConnectScreen;
class FriendPanel;
class JamPanel;   // "Birlikte dinle" (app/Together.h)

// ---- Internet radio (defined in app/RadioPage.cpp) --------------------------------------------------------------
// Shared by the Radyo page and the live displays (player bar, queue, Now Playing, mini player). A station plays as a
// track with a "radio:<uuid>" id (app/InternetRadio.h).
// Station logo (favicon) letterboxed on a neutral surface, never cropped; a radio placeholder while it loads or when
// it is missing / broken.
void drawStationArt(Canvas& c, const std::vector<catalog::Image>& images, const Rect& r, float radius,
                    Priority prio = Priority::High);
// Context menu of a station: Favorilere ekle / kaldır, Web sitesini aç, Yayın adresini kopyala.
void showStationMenu(const catalog::Track& station, gfx::Point windowPos);
// Song titles (ICY StreamTitle) heard on the playing station this session, newest first; reset by another station.
struct HeardTitle {
    int64_t at = 0;       // unix seconds
    std::wstring title;
};
const std::vector<HeardTitle>& heardTitles();
// The "CANLI" pill of the live displays: a dot (pulsing while `playing`) and the mono label, vertically centered on
// `leftCenter`. Returns its width.
float drawLiveBadge(Canvas& c, gfx::Point leftCenter, bool playing);

class TitleBar : public ui::Widget {
public:
    TitleBar();
    void setNowPlayingMode(bool on);
    void layout() override;
    void paint(Canvas& c) override;

private:
    ui::Button *back_, *forward_, *collapse_;
    ui::Button* friends_;   // "Arkadaş etkinliği" (only with a Spotify session)
    ui::Button *min_, *max_, *close_;
    bool nowPlaying_ = false;
};

class SidebarRow;   // a playlist or folder row (Shell.cpp)

class Sidebar : public ui::Widget, public DropTarget {
public:
    Sidebar();
    void refresh();          // playlists / counts / folders changed
    void syncActive();       // router changed
    // The icon-only rail (Shell::setSidebarCollapsed): nav items as icons with their names as tooltips, playlists as
    // cover tiles. `progress` is the collapse animation (0 = expanded .. 1 = rail) the layout morphs with; the rows
    // switch to tiles half way.
    void setCollapsed(bool on);
    void setRailProgress(float progress);
    void layout() override;
    void paint(Canvas& c) override;

    // Drag and drop: songs onto Liked Songs or a playlist the user can add to. A closed folder opens while they rest
    // on it; the list scrolls near its edges.
    bool dragOver(const DragPayload& payload, gfx::Point windowPos) override;
    void dragLeave() override;
    void drop(const DragPayload& payload, gfx::Point windowPos) override;

private:
    void setFolderOpen(const std::string& folderId, bool open);   // Settings::expandedFolders, then refresh()
    SidebarRow* rowAt(gfx::Point windowPos) const;
    std::vector<ui::Button*> navButtons() const;   // the page entries, top to bottom (Ayarlar last)
    float labelOpacity() const;                     // names fade out as the rail takes the room

    ui::Button *home_, *search_, *library_, *downloads_, *local_, *radio_, *podcasts_, *releases_, *stats_, *settings_,
        *newPlaylist_;   // releases_: "Yeni çıkanlar", only with a Spotify session
    ui::ScrollView* list_;
    ui::Column* listCol_;
    std::vector<std::pair<SidebarRow*, Route>> items_;   // folders: {Library, "folder:<id>"}
    float listLabelY_ = 0;                               // the "Çalma listeleri" label (below the visible nav items)
    SidebarRow* dropRow_ = nullptr;                      // highlighted drop target
    SidebarRow* restingFolder_ = nullptr;                // closed folder under a drag, since restingSince_
    double restingSince_ = 0, lastScrollTick_ = 0;
    std::vector<std::pair<ui::Button*, std::wstring>> navNames_;   // tooltips of the rail
    float rail_ = 0;           // collapse progress (setRailProgress)
    bool collapsed_ = false;   // setCollapsed
    bool compact_ = false;     // the rows are tiles (rail_ past half way)
    Lifetime life_;
};

// The sidebar's right edge (6 DIPs over its hairline, IDC_SIZEWE): drag to resize, double click for the default width.
class SidebarEdge;

class PlayerBar : public ui::Widget {
public:
    PlayerBar();
    void sync();                         // player state changed
    void setNowPlayingMode(bool on);
    void layout() override;
    void paint(Canvas& c) override;
    ui::Slider* seek() { return seek_; }

private:
    ui::Slider* seek_;
    ui::Button *shuffle_, *prev_, *next_, *repeat_, *heart_;
    ui::PlayButton* play_;
    ui::Button *lyrics_, *queue_, *mini_, *expand_, *sleep_, *speed_;
    ui::Button *jam_, *jamLabel_;   // "Birlikte dinle": the icon, or the session pill ("Birlikte dinleniyor · Ayşe")
    bool lockedHover_ = false;      // the pointer is on a transport control a guest may not use (its tooltip says why)
    ui::Knob* knob_;
    gfx::Text title_, artist_, time_;
    Rect infoRect_{}, artRect_{}, timeRect_{};
    bool nowPlaying_ = false;
    bool infoHover_ = false;
    bool live_ = false;      // the current item is an internet radio station (no times, no seeking)

public:
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return infoHover_ ? IDC_HAND : IDC_ARROW; }
    // A guest's disabled transport controls still explain themselves: the bar takes the pointer over them.
    Widget* hitTest(gfx::Point pos) override;
    std::wstring tooltip() const override;
};

class QueuePanel : public ui::Widget, public DropTarget {
public:
    QueuePanel();
    void sync();
    float preferredHeight(float) override { return rect().h; }
    void layout() override;
    void paint(Canvas& c) override;
    bool onWheel(float dy, float, const ui::MouseEvent&) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseLeave() override { hover_ = -1; invalidate(); }
    LPCWSTR cursor() const override { return hover_ >= 0 ? IDC_HAND : IDC_ARROW; }
    // Drag and drop: songs (or files from Explorer) go into the queue where they are dropped.
    bool dragOver(const DragPayload& payload, gfx::Point windowPos) override;
    void dragLeave() override;
    void drop(const DragPayload& payload, gfx::Point windowPos) override;

private:
    int dropIndexAt(gfx::Point windowPos) const;   // order index the dropped songs go in front of
    int rowAt(gfx::Point p) const;       // order index or -1
    Rect rowRect(int orderIndex) const;
    float listTop() const;
    float maxScroll() const;             // the upcoming rows below the current one
    void saveAsPlaylist();               // "Kuyruğu kaydet": current + upcoming, in play order
    gfx::Text title_, meta_, blockedBadge_;
    ui::Button* save_;
    ui::SmoothScroll scroll_;            // the upcoming rows (ui/ScrollPhysics: wheel glide, touchpad 1:1)
    int hover_ = -1;
    int dragFrom_ = -1, dragTo_ = -1;
    float dragStartY_ = 0;
    bool dragging_ = false;
    int dropAt_ = -1;                    // songs dragged in from elsewhere: order index of the insertion line
    double lastScrollTick_ = 0;
};

// Hosts the current page with the enter transition (opacity + 16 px rise, 360 ms decelerate).
class PageHost : public ui::Widget {
public:
    PageHost() { clipsChildren = true; hitTestVisible = false; }
    void show(std::unique_ptr<Page> page);
    Page* page() const { return page_; }
    void layout() override;
    void paint(Canvas& c) override;

private:
    Page* page_ = nullptr;
    ui::Anim enter_;
};

class Shell : public ui::Widget {
public:
    Shell();
    void layout() override;
    void paint(Canvas& c) override;

    void setNowPlaying(bool on);
    bool nowPlaying() const { return nowPlaying_; }
    void setQueueOpen(bool on);
    bool queueOpen() const { return queueOpen_; }
    // The friend activity panel takes the queue's place: opening one closes the other.
    void setFriendsOpen(bool on);
    bool friendsOpen() const { return friendsOpen_; }
    // "Birlikte dinle" takes the same slot (opening it closes the queue and the friend activity).
    void setJamOpen(bool on);
    bool jamOpen() const { return jamOpen_; }
    void setConnect(bool on);                     // full-window Spotify connect overlay
    bool connectMode() const { return connectMode_; }

    // Sidebar width. Expanded it is Settings::sidebarWidth (200..420 DIPs, dragged at its right edge; the page keeps at
    // least 400 DIPs, so a narrow window shows it narrower); collapsed it is the 72 DIP icon rail (Ctrl+B, the edge
    // dragged below 150 DIPs). Collapsing / expanding animates the width (instant with reduce motion).
    float sidebarWidth() const;                   // as laid out (mid-animation: in between)
    bool sidebarCollapsed() const { return sideCollapsed_; }
    void setSidebarCollapsed(bool on);            // saved to Settings
    void beginSidebarDrag();                      // the edge pressed: a drag into the rail keeps today's width
    void dragSidebar(float width);                // the edge dragged to `width`: resize, collapse below / expand above
    void resetSidebar();                          // double click on the edge: expanded, 240 DIPs; saved
    void saveSidebar();                           // end of a drag: width + state to Settings (when changed)

    TitleBar* titleBar() const { return titleBar_; }
    Sidebar* sidebar() const { return sidebar_; }
    PlayerBar* playerBar() const { return playerBar_; }
    PageHost* pageHost() const { return pageHost_; }
    QueuePanel* queuePanel() const { return queue_; }
    FriendPanel* friendPanel() const { return friends_; }
    JamPanel* jamPanel() const { return jam_; }
    NowPlayingView* nowPlayingView() const { return nowPlayingView_; }

private:
    TitleBar* titleBar_;
    Sidebar* sidebar_;
    PageHost* pageHost_;
    QueuePanel* queue_;
    FriendPanel* friends_;
    JamPanel* jam_;
    PlayerBar* playerBar_;
    NowPlayingView* nowPlayingView_;
    ConnectScreen* connect_ = nullptr;
    SidebarEdge* edge_ = nullptr;
    float sideW_ = 240;           // expanded width the user chose
    float sideDragFromW_ = 240;   // sideW_ when the current drag began (restored when it ends in the rail)
    float sideMaxW_ = 420;        // the widest the window allows now (layout)
    bool sideCollapsed_ = false;
    bool sideDevOverride_ = false;   // SHADETUBE_SIDEBAR_W: this run's width, never saved
    ui::Anim railAnim_;           // 0 = expanded .. 1 = rail
    bool nowPlaying_ = false, queueOpen_ = false, friendsOpen_ = false, connectMode_ = false;
    bool jamOpen_ = false;
    bool npActive_ = false;   // Now Playing holds its resources (activate() ran, deactivate() not yet)
    ui::Anim npAnim_, queueAnim_;
    Lifetime life_;
};

} // namespace st::app
