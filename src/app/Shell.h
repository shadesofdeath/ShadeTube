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
    ui::Button *min_, *max_, *close_;
    bool nowPlaying_ = false;
};

class SidebarRow;   // a playlist or folder row (Shell.cpp)

class Sidebar : public ui::Widget, public DropTarget {
public:
    Sidebar();
    void refresh();          // playlists / counts / folders changed
    void syncActive();       // router changed
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

    ui::Button *home_, *search_, *library_, *downloads_, *local_, *radio_, *podcasts_, *stats_, *settings_, *newPlaylist_;
    ui::ScrollView* list_;
    ui::Column* listCol_;
    std::vector<std::pair<SidebarRow*, Route>> items_;   // folders: {Library, "folder:<id>"}
    SidebarRow* dropRow_ = nullptr;                      // highlighted drop target
    SidebarRow* restingFolder_ = nullptr;                // closed folder under a drag, since restingSince_
    double restingSince_ = 0, lastScrollTick_ = 0;
    Lifetime life_;
};

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
    ui::Button *lyrics_, *queue_, *mini_, *expand_, *sleep_;
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
    void saveAsPlaylist();               // "Kuyruğu kaydet": current + upcoming, in play order
    gfx::Text title_, meta_, blockedBadge_;
    ui::Button* save_;
    ui::Anim scroll_;
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
    void setConnect(bool on);                     // full-window Spotify connect overlay
    bool connectMode() const { return connectMode_; }

    TitleBar* titleBar() const { return titleBar_; }
    Sidebar* sidebar() const { return sidebar_; }
    PlayerBar* playerBar() const { return playerBar_; }
    PageHost* pageHost() const { return pageHost_; }
    QueuePanel* queuePanel() const { return queue_; }
    NowPlayingView* nowPlayingView() const { return nowPlayingView_; }

private:
    TitleBar* titleBar_;
    Sidebar* sidebar_;
    PageHost* pageHost_;
    QueuePanel* queue_;
    PlayerBar* playerBar_;
    NowPlayingView* nowPlayingView_;
    ConnectScreen* connect_ = nullptr;
    bool nowPlaying_ = false, queueOpen_ = false, connectMode_ = false;
    bool npActive_ = false;   // Now Playing holds its resources (activate() ran, deactivate() not yet)
    ui::Anim npAnim_, queueAnim_;
};

} // namespace st::app
