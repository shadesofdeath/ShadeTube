#pragma once
// Application chrome: title bar, sidebar, player bar, queue panel, page host and the root layout.
#include "app/AppContext.h"
#include "app/Components.h"
#include "app/Router.h"
#include "ui/Controls.h"
#include "ui/Layout.h"

#include <memory>

namespace st::app {

class NowPlayingView;
class ConnectScreen;

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

class Sidebar : public ui::Widget {
public:
    Sidebar();
    void refresh();          // playlists / counts changed
    void syncActive();       // router changed
    void layout() override;
    void paint(Canvas& c) override;

private:
    ui::Button *home_, *search_, *library_, *downloads_, *local_, *stats_, *settings_, *newPlaylist_;
    ui::ScrollView* list_;
    ui::Column* listCol_;
    std::vector<std::pair<ui::Button*, Route>> items_;
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

public:
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override { return infoHover_ ? IDC_HAND : IDC_ARROW; }
};

class QueuePanel : public ui::Widget {
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

private:
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
