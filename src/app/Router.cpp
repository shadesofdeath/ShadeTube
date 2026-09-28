#include "app/Router.h"

namespace st::app {

void Router::navigate(const Route& route, bool replace) {
    if (route == current() && !replace) {
        // Re-selecting the current page scrolls it to the top.
        if (currentPage) {
            if (auto* p = currentPage()) p->restoreScroll(0);
        }
        return;
    }
    if (currentPage) {
        if (auto* p = currentPage()) history_[index_].scroll = p->scrollOffset();
    }
    if (replace) {
        history_[index_] = {route, 0};
    } else {
        history_.resize(index_ + 1);
        history_.push_back({route, 0});
        ++index_;
        if (history_.size() > 64) {   // bounded history
            history_.erase(history_.begin());
            --index_;
        }
    }
    show(0);
}

void Router::back() {
    if (!canBack()) return;
    if (currentPage) {
        if (auto* p = currentPage()) history_[index_].scroll = p->scrollOffset();
    }
    --index_;
    show(history_[index_].scroll);
}

void Router::forward() {
    if (!canForward()) return;
    if (currentPage) {
        if (auto* p = currentPage()) history_[index_].scroll = p->scrollOffset();
    }
    ++index_;
    show(history_[index_].scroll);
}

void Router::show(float scroll) {
    auto page = factory_(history_[index_].route);
    if (!page) return;
    Page* raw = page.get();
    if (onShow) onShow(std::move(page));
    if (scroll > 0) raw->restoreScroll(scroll);
    raw->onShown();
    if (onChanged) onChanged();
}

} // namespace st::app
