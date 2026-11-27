#pragma once
// Application chrome: title bar, sidebar, player bar, queue panel, page host and the root layout.
#include "app/AppContext.h"
#include "app/Components.h"
#include "app/DragDrop.h"
#include "app/PageTransition.h"
#include "app/PageWidgets.h"
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
    ui::Widget* account_;   // the source tag as the account menu's button (app/Accounts: AccountChip)
    bool nowPlaying_ = false;
};

class SidebarRow;   // a playlist or folder row (Shell.cpp)
class NavBox;       // the page entries in the sidebar's nav scroller (Shell.cpp)
class NavGroup;     // the "Daha fazla" header that folds the less used pages (Shell.cpp)

// Height budget (top to bottom): the main pages (Ana Sayfa, Ara, Kitaplık), the "Daha fazla" group (İndirilenler,
// Yerel dosyalar, Radyo, Podcastler, Yeni çıkanlar, İstatistikler; folded, the current page among them stays), the
// playlists, Ayarlar. The playlists always keep room for kMinListRows rows: on a short sidebar the nav first gets
// tighter (also with the compact list density), then scrolls on its own. The group's state is Settings::sidebarMore
// (-1 = automatic: open while that leaves the list kAutoListRows rows).
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

    // "Daha fazla": open / fold the less used pages (remembered in Settings::sidebarMore). Animated unless reduce motion.
    bool moreOpen() const { return moreOpen_; }
    void setMoreOpen(bool open);

    static constexpr int kMinListRows = 5, kAutoListRows = 8;

private:
    friend class NavBox;
    friend class NavGroup;
    void setFolderOpen(const std::string& folderId, bool open);   // Settings::expandedFolders, then refresh()
    SidebarRow* rowAt(gfx::Point windowPos) const;
    std::vector<ui::Button*> navButtons() const;   // the page entries, top to bottom (Ayarlar last)
    std::vector<ui::Button*> groupButtons() const; // the pages "Daha fazla" folds (those available now)
    ui::Button* pinnedButton() const;              // the current page when it is in the folded group, else nullptr
    float labelOpacity() const;                     // names fade out as the rail takes the room
    void layoutNav(float rowH, float gap, float groupH);   // positions the entries in navBox_ (content space)
    float navContentHeight(float rowH, float gap, float groupH, float open) const;

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
    ui::ScrollView* nav_ = nullptr;   // the page entries; scrolls only when even the tight nav would squeeze the list
    NavBox* navBox_ = nullptr;
    NavGroup* group_ = nullptr;
    bool moreOpen_ = true;            // as shown (the setting, or the automatic choice)
    ui::Anim moreAnim_{1.f};          // 0 = folded .. 1 = open
    bool tight_ = false;              // the tighter nav metrics are in use (layout)
    bool moreSettling_ = false;       // one more layout after the group's animation ends
};

// The sidebar's right edge (6 DIPs over its hairline, IDC_SIZEWE): drag to resize, double click for the default width.
class SidebarEdge;

// The player bar (88 DIP): track info on the left, the transport row above the wave seek row in the center (elapsed
// time · wave · total time), the other controls on the right. Every control uses the thin outline icon set
// (assets/icons, as the whole app) and the outline play button; a track change slides the info over (TrackSwap).
class PlayerBar : public ui::Widget {
public:
    PlayerBar();
    void sync();                         // player state changed
    void setNowPlayingMode(bool on);
    void layout() override;
    void paint(Canvas& c) override;
    ui::Slider* seek() { return seek_; }

private:
    // Cover, title and artist of the current track (or of the outgoing one during a track change).
    void paintInfo(Canvas& c, bool outgoing);

    ui::Slider* seek_;
    ui::Button *shuffle_, *prev_, *next_, *repeat_, *heart_;
    ui::PlayButton* play_;
    ui::Button *lyrics_, *queue_, *mini_, *expand_, *sleep_, *speed_;
    ui::Button *jam_, *jamLabel_;   // "Birlikte dinle": the icon, or the session pill ("Birlikte dinleniyor · Ayşe")
    ui::Button* video_ = nullptr;   // "Video modu" (app/VideoMode): songs only
    ui::Button* viz_ = nullptr;     // the visualizer (app/Visualizer): only while the center column keeps 380 DIP
    Rect stripRect_{};              // Now Playing's spectrum strip in place of the track info (a click opens the visualizer)
    bool lockedHover_ = false;      // the pointer is on a transport control a guest may not use (its tooltip says why)
    ui::Knob* knob_;
    gfx::Text title_, artist_, elapsed_, total_;
    Rect infoRect_{}, artRect_{}, elapsedRect_{}, totalRect_{};
    bool nowPlaying_ = false;
    bool infoHover_ = false;
    bool live_ = false;      // the current item is an internet radio station (no times, no seeking)
    bool previewHeld_ = false;   // a song preview holds the current item (app/Preview): the bar looks paused
    // Track change: what the bar showed for the previous track, sliding out while the new one slides in.
    TrackSwap swap_;
    FadingArt art_;
    struct Shown {
        std::vector<catalog::Image> images;
        bool live = false, recommended = false;
        float titleW = 0;
    };
    Shown shown_, old_;
    gfx::Text oldTitle_, oldArtist_;
    float heartFrom_ = -1;   // the heart's x for the outgoing track (it slides to the new title's end)
    bool heartSliding_ = false;   // laid out mid-slide: one more layout after the transition puts it in place
    std::string likedId_;         // the track the heart showed at the last sync() and whether it was liked: a change
    bool likedWas_ = false;       // of the same track plays the like feedback

public:
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return infoHover_ ? IDC_HAND : IDC_ARROW; }
    // A guest's disabled transport controls still explain themselves: the bar takes the pointer over them.
    Widget* hitTest(gfx::Point pos) override;
    std::wstring tooltip() const override;
};

// The queue: "Şimdi çalıyor" (the playing song, its progress), then the upcoming songs in sections: the songs the user
// queued ("Sırada") and the rest of what plays ("Sıradaki · <kaynak>"), each run with its own label
// (app/TrackColumns queuelist::Sections). Rows can be dragged into a new place (the row follows the pointer, a line shows
// where it lands), hovered rows offer "Sıradan kaldır", the header has "Kuyruğu kaydet" and "Sırayı temizle". The
// recommended and listen-together marks are the track table's badges (app/Components badge::).
class QueuePanel : public ui::Widget, public DropTarget, public PreviewSource {
public:
    QueuePanel();
    void sync();
    // Now Playing's Sıradaki tab: no panel background or title row; the count and the actions share the "ŞİMDİ ÇALIYOR"
    // row at the top.
    void setEmbedded(bool on) { embedded_ = on; }
    float preferredHeight(float) override { return rect().h; }
    void layout() override;
    void paint(Canvas& c) override;
    bool onWheel(float dy, float, const ui::MouseEvent&) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return hover_ >= 0 ? IDC_HAND : IDC_ARROW; }
    // Drag and drop: songs (or files from Explorer) go into the queue where they are dropped.
    bool dragOver(const DragPayload& payload, gfx::Point windowPos) override;
    void dragLeave() override;
    void drop(const DragPayload& payload, gfx::Point windowPos) override;
    // Song preview (app/Preview): an upcoming row (recommendations too); not the playing one.
    std::optional<PreviewHit> previewAt(gfx::Point windowPos, bool keyboard) override;
    // Keyboard (one Tab stop, the track table's keys): Up / Down / PageUp / PageDown / Home / End move the cursor over
    // the upcoming rows, Enter plays the row, Delete takes it out of the queue, Alt+Up / Alt+Down move it, menu key /
    // Shift+F10 open its menu. The Window draws the focus ring around the cursor's row.
    bool onKeyDown(const ui::KeyEvent& e) override;
    Rect focusRect() const override;

private:
    void showRowMenu(int orderIndex, gfx::Point windowPos);
    void revealCursor();                 // scrolls the upcoming rows so the cursor's row is in view
    int cursorIndex() const;             // the cursor's order index, clamped to the upcoming rows (-1: none)
    int dropIndexAt(gfx::Point windowPos) const;   // order index the dropped songs go in front of
    int rowAt(gfx::Point p) const;       // order index or -1
    Rect rowRect(int orderIndex) const;
    Rect removeRect(const Rect& row) const;   // the hovered row's "Sıradan kaldır"
    float listTop() const;
    float maxScroll() const;             // the upcoming rows below the current one
    void rebuildSections();              // sections_ from the player's order (cheap: one pass over the upcoming slots)
    void saveAsPlaylist();               // "Kuyruğu kaydet": current + upcoming, in play order
    std::wstring sectionLabel(bool queued) const;
    gfx::Text title_, meta_, nowLabel_;
    ui::Button *save_, *clear_, *close_;   // close_: the side panel only (the friend / jam panels' header)
    MessagePanel* empty_;                // the empty queue (compact empty state)
    queuelist::Sections sections_;
    int secPos_ = -2, secSize_ = -1;     // the player's slot / order size sections_ were built for
    uint64_t secGen_ = 0;
    ui::SmoothScroll scroll_;            // the upcoming rows (ui/ScrollPhysics: wheel glide, touchpad 1:1)
    int hover_ = -1;
    int cursor_ = -1;                    // keyboard cursor: upcoming slot k (0 = the row after the playing one)
    bool hoverRemove_ = false;
    ui::HoverFade hoverFade_;            // upcoming rows' hover fades in / out (ui/Micro)
    int dragFrom_ = -1, dragTo_ = -1;    // order indices: the row being moved and the slot it goes to
    float dragStartY_ = 0, dragY_ = 0;   // the press and the pointer (local y)
    bool dragging_ = false;
    ui::Anim dragLineY_;                 // the insertion line glides between slots
    int dropAt_ = -1;                    // songs dragged in from elsewhere: order index of the insertion line
    double lastScrollTick_ = 0;
    bool embedded_ = false;
    float oy() const { return embedded_ ? -44.f : 0.f; }   // embedded: everything moves up into the title row's place
};

// Hosts the current page and the page transition (app/PageTransition): the replaced page fades / shifts out while the
// new one slides in, a cover that opened the page flies into its header.
class PageHost : public ui::Widget {
public:
    PageHost() { clipsChildren = true; hitTestVisible = false; }
    void show(std::unique_ptr<Page> page);
    Page* page() const { return page_; }
    void layout() override;
    void paint(Canvas& c) override;

private:
    Page* page_ = nullptr;
    PageTransition transition_;
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
    ui::Anim npAnim_;
    // The side panels (queue, friend activity, "Birlikte dinle") open and close with the overlay family's motion: the
    // page takes its new width at once, the panel fades in sliding 16 DIP from the right (motion::medium, decelerate)
    // and a closed one fades out drifting 8 DIP right over the page (140 ms, accelerate); none with Hareketi azalt or
    // Windows' animation effects off (ui::overlay::duration).
    ui::Widget* sideShown_ = nullptr;     // the panel on screen (layout)
    ui::Widget* sideLeaving_ = nullptr;   // the panel fading out (hidden, painted by paint())
    ui::Anim sideIn_{1.f}, sideOut_;
    void paintSidePanel(Canvas& c, ui::Widget* panel, float t, bool leaving);
    Lifetime life_;
};

} // namespace st::app
