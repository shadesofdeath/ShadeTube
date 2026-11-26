#pragma once
// Pages (one per route) + the Spotify connect screen.
#include "app/Components.h"
#include "app/Router.h"
#include "ui/Layout.h"
#include "ui/TextBox.h"

#include <memory>

namespace st::app {

class CollapseBar;
class HeroHeader;

// Base: vertical scroll page with a Column, loading skeleton, error state and deferred scroll restore.
class ScrollPage : public Page {
public:
    ScrollPage();
    void layout() override;
    void paint(Canvas& c) override;
    float scrollOffset() const override;
    void restoreScroll(float y) override;

protected:
    // A page whose header collapses into a compact bar as it scrolls (app/CollapsingHeader): call once in the
    // constructor, then attachHeader() whenever the content is built (resetContent() detaches it).
    void enableCollapse();
    // `dock`: what docks under the bar (nullptr: the content after the header); `columns`: a track list whose column
    // header docks with it.
    void attachHeader(HeroHeader* header, ui::Widget* dock, TrackTable* columns = nullptr);
    CollapseBar* collapse_ = nullptr;

    // Replace content with an error panel ("Tekrar dene" calls retry).
    void showError(const std::wstring& message, std::function<void()> retry);
    void showSkeleton(int rows = 8);   // a collection page loading: header + track rows (SkeletonBlock)
    // Other pages loading in their own shape (app/PageWidgets skeletons): a page title over a grid of cards, over
    // shelves (sections of cards), or over track rows.
    enum class Loading { Cards, Shelves, Rows };
    void showLoading(Loading shape);
    void contentReady();   // content is built: applies a pending scroll restore (now or when restoreScroll comes)
    ui::Column* resetContent(float gap = gfx::metrics::sectionGap);
    // coverSlot() of a detail page still showing its loading skeleton: where its header's cover will be (not ready).
    bool loadingCoverSlot(SharedCover& out) const;
    // coverSlot() of a page with an attached collapsing header: its HeaderCover where it is now (CollapseBar::coverSlot).
    bool headerCoverSlot(SharedCover& out);

    ui::ScrollView* scroll_;
    ui::Column* col_;
    Lifetime life_;
    float pendingScroll_ = -1;
    bool built_ = false;   // contentReady() ran since the last skeleton / error

private:
    void applyPendingScroll();
    bool devScrolled_ = false;   // SHADETUBE_HEADER_SCROLL applied (dev)
};

std::unique_ptr<Page> createPage(const Route& route);

} // namespace st::app
