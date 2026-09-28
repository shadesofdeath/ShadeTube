#pragma once
// Pages (one per route) + the Spotify connect screen.
#include "app/Components.h"
#include "app/Router.h"
#include "ui/Layout.h"
#include "ui/TextBox.h"

#include <memory>

namespace st::app {

// Base: vertical scroll page with a Column, loading skeleton, error state and deferred scroll restore.
class ScrollPage : public Page {
public:
    ScrollPage();
    void layout() override;
    float scrollOffset() const override;
    void restoreScroll(float y) override;

protected:
    // Replace content with an error panel ("Tekrar dene" calls retry).
    void showError(const std::wstring& message, std::function<void()> retry);
    void showSkeleton(int rows = 8);
    void contentReady();   // content is built: applies a pending scroll restore (now or when restoreScroll comes)
    ui::Column* resetContent(float gap = gfx::metrics::sectionGap);

    ui::ScrollView* scroll_;
    ui::Column* col_;
    Lifetime life_;
    float pendingScroll_ = -1;
    bool built_ = false;   // contentReady() ran since the last skeleton / error

private:
    void applyPendingScroll();
};

std::unique_ptr<Page> createPage(const Route& route);

} // namespace st::app
