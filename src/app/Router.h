#pragma once
// Navigation between pages with history (back/forward, mouse buttons, Alt+arrows).
// History stores routes + scroll offsets, never widgets: pages are rebuilt on navigation, so only the
// visible page holds memory.
#include "ui/Anim.h"
#include "ui/Widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::app {

enum class RouteKind { Home, Search, Library, Liked, Playlist, Album, Artist, Downloads, Settings, Stats, LocalFiles, Radio };

struct Route {
    RouteKind kind = RouteKind::Home;
    std::string id;       // playlist/album/artist id, search query...
    bool operator==(const Route& o) const { return kind == o.kind && id == o.id; }
};

class Page : public ui::Widget {
public:
    // Scroll position persisted in history.
    virtual float scrollOffset() const { return 0; }
    virtual void restoreScroll(float) {}
    virtual void onShown() {}
};

class Router {
public:
    using Factory = std::function<std::unique_ptr<Page>(const Route&)>;
    explicit Router(Factory factory) : factory_(std::move(factory)) {}

    void navigate(const Route& route, bool replace = false);
    void back();
    void forward();
    bool canBack() const { return index_ > 0; }
    bool canForward() const { return index_ + 1 < static_cast<int>(history_.size()); }
    const Route& current() const { return history_[index_].route; }

    // Host widget that displays the current page (with the page transition).
    ui::Widget* host() const { return host_; }
    void setHost(ui::Widget* host) { host_ = host; }
    std::function<void(std::unique_ptr<Page>)> onShow;   // provided by the shell (PageHost)
    std::function<void()> onChanged;                      // sidebar highlight etc.
    std::function<Page*()> currentPage;

private:
    struct Entry {
        Route route;
        float scroll = 0;
    };
    void show(float scroll);
    Factory factory_;
    std::vector<Entry> history_{{Route{}, 0}};
    int index_ = 0;
    ui::Widget* host_ = nullptr;
};

} // namespace st::app
