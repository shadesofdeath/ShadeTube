#pragma once
// Collapsing page headers (docs/DEVELOPMENT.md 3.x). The header of an album, playlist, Liked Songs, smart list,
// podcast show and artist page collapses into a compact sticky bar as the page scrolls: the cover shrinks into the
// bar's corner, the title scales down beside it, the rest of the header dissolves into the bar and the list's column
// header docks under it. Everything is a function of the scroll offset of the frame (app/HeaderCollapse), evaluated at
// paint time: no timers, no relayout of the page while it scrolls.
//
// Pieces: HeroHeader is the base of the page headers (title, cover, backdrop tint, the content that dissolves);
// CollapseBar is the page's overlay (a child of the page above its ScrollView, ScrollPage::enableCollapse) that paints
// the backdrop behind the content, the moving cover and title, the bar's surface, play / shuffle and the docked column
// header; HeaderCover is the cover itself, a widget the bar moves and shrinks.
#include "app/Components.h"
#include "app/HeaderCollapse.h"
#include "app/PageTransition.h"

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace st::ui {
class ScrollView;
}

namespace st::app {

class CollapseBar;

// Base of the page headers that collapse. A header lays out its texts in layout() (header-local coordinates, valid at
// scroll offset 0): titleBox_, titleSize_, contentTop_; it paints everything but the cover and the title in
// paintContent() (the bar draws those two), under a mask that dissolves it into the bar.
class HeroHeader : public ui::Widget {
public:
    static constexpr float kCoverSize = 232;
    HeroHeader();

    // ---- read by the bar -----------------------------------------------------------------------------------------
    // The cover's slot (header-local, at scroll offset 0); empty = no cover (artist: an avatar comes in with the bar).
    virtual Rect coverSlot() const { return {0, 0, kCoverSize, kCoverSize}; }
    // Draws the cover into `r`, always the expanded slot's size (the bar scales the drawing down, so images keep the
    // bitmap they were loaded at); `radius` is in r's units.
    virtual void drawCover(Canvas& c, const Rect& r, float radius);
    // The bar's avatar of a header without a cover (artist): a circle filling `r`.
    virtual void drawAvatar(Canvas& c, const Rect& r) { (void)c, (void)r; }
    // The cover for a page transition (app/PageTransition) as `owner` (the HeaderCover) shows it at `local` (its rect):
    // the images by default; Liked Songs and smart lists paint theirs.
    virtual SharedCover sharedCover(const ui::Widget& owner, const Rect& local);
    // Behind the page's content, in page coordinates (`page` = the page's rect there): the tint gradient. The artist
    // page adds its banner.
    virtual void paintBackdrop(Canvas& c, const Rect& page, const collapse::Frame& f);
    // The tint behind the docked bar (clipped to it by the bar): the same gradient as the backdrop's.
    virtual void paintBarTint(Canvas& c, const Rect& page, const collapse::Frame& f) { paintTint(c, page, f, 1.f); }
    virtual float backdropHeight() const { return 420; }

    gfx::Text& titleText() { return title_; }
    // The title on one line (ellipsis) in the expanded title's style and size: the bar draws it scaled down, so the
    // cross-fade from the expanded layout shows no second outline (a one-line title is the same layout).
    gfx::Text& compactTitle();
    const Rect& titleBox() const { return titleBox_; }   // the expanded title's layout box (header-local)
    float titleSize() const { return titleSize_; }
    float contentTop() const { return contentTop_; }     // topmost content that dissolves (header-local)
    // The backdrop tint: the cover's color (cross-faded in when it arrives), as a translucent wash that keeps the text
    // on it readable (collapse::wash).
    Color tint();

    // The header's own play / shuffle (the bar's buttons mirror them: same onClick, same playing state).
    ui::PlayButton* play_ = nullptr;
    ui::Button* shuffle_ = nullptr;
    // The page's collection is playing: both play buttons show pause (unset: play_ keeps its own state).
    std::function<bool()> isPlaying;
    // A playlist the user may edit: the cover and the title open "Ayrıntıları düzenle".
    std::function<void()> onEditDetails;

    void paint(Canvas& c) final;

protected:
    virtual void paintContent(Canvas& c) { (void)c; }
    void setTitle(const std::wstring& text);
    void setTitleStyle(const gfx::TextStyle& style, bool wrap);
    // Picks the first of `sizes` at which the title fits `width` (sizes above `oneLineAbove` on one line, the others on
    // at most two), sets titleSize_ and returns the title's height.
    float fitTitle(float width, std::initializer_list<float> sizes, float oneLineAbove, const gfx::TextStyle& base);
    // Tint from the cover (the color the image cache extracts; until it arrives, and when it has none, the app's
    // accent) / a fixed color (nullopt = the app's accent, live).
    void setTintImages(const std::vector<catalog::Image>& images);
    void setTintColor(std::optional<Color> color);
    void paintTint(Canvas& c, const Rect& page, const collapse::Frame& f, float amount);

    std::vector<catalog::Image> images_;   // the cover (drawCover's default) and the tint's source
    Placeholder placeholder_ = Placeholder::Album;
    Rect titleBox_{};
    float titleSize_ = 88;
    float contentTop_ = 0;

private:
    friend class CollapseBar;
    void tintArrived(std::optional<Color> c);

    CollapseBar* bar_ = nullptr;
    gfx::Text title_, compact_;
    std::string tintUrl_;
    std::optional<Color> tintFrom_, tintTo_;   // nullopt = the app's accent
    ui::Anim tintFade_{1.f};
    bool painted_ = false;
    Lifetime tintLife_;
};

// The cover of the attached header, moved and shrunk by its bar. At scroll offset 0 its rect is the header's cover slot
// (232 DIP square at the header's top-left): the page's Page::coverSlot for the page transitions, which fly a card's
// cover into it and back from wherever it is. It is the SharedCover's owner: it skips its artwork and shadow while
// pagetx::presence() is 0 (the cover flies) and shows the shadow at that strength as it lands. Pages without a cover
// (artist) use it for the bar's avatar.
class HeaderCover : public ui::Widget {
public:
    explicit HeaderCover(CollapseBar& bar) : bar_(bar) {}
    void paint(Canvas& c) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseEnter() override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override;

private:
    bool editable() const;
    CollapseBar& bar_;
    bool hover_ = false;
};

class CollapseBar : public ui::Widget {
public:
    explicit CollapseBar(ui::ScrollView* scroll);

    // The content's header and what docks under the bar: `dock`'s top edge (nullptr: 28 DIP below the header);
    // `columns`: a track list whose column header docks with it (its copy is painted in the bar).
    void attach(HeroHeader* header, ui::Widget* dock, TrackTable* columns = nullptr);
    void detach() { attach(nullptr, nullptr, nullptr); }
    HeroHeader* header() const { return header_; }
    HeaderCover* cover() const { return cover_; }
    const collapse::Frame& frame() const { return frame_; }
    bool active() const { return valid_; }
    // Page::coverSlot of a page with this bar: the header's cover where it is now (its slot at offset 0, the shrinking
    // or the bar's thumbnail when scrolled), owned by the HeaderCover; false without a header or a cover.
    bool coverSlot(SharedCover& out);

    // Once per frame, before the content (ScrollPage::paint, canvas in page coordinates): evaluates the collapse at
    // the scroll offset of this frame, places the cover and the buttons, sets the ScrollView's topInset and paints the
    // backdrop behind the content.
    void paintBackdrop(Canvas& c);
    // Page y -> y in the parent-content space of `w` (a widget inside the page's content).
    float pageToLocalY(const ui::Widget& w, float pageY) const;

    void paint(Canvas& c) override;
    Widget* hitTest(gfx::Point p) override;
    bool onWheel(float dy, float dx, const ui::MouseEvent& e) override;
    bool onMouseDown(const ui::MouseEvent& e) override;
    void onMouseUp(const ui::MouseEvent& e) override;
    void onMouseMove(const ui::MouseEvent& e) override;
    void onMouseLeave() override;
    LPCWSTR cursor() const override;

private:
    void update();
    void paintTitle(Canvas& c);
    Rect pageRect() const;

    ui::ScrollView* scroll_;
    HeroHeader* header_ = nullptr;
    ui::Widget* dock_ = nullptr;
    TrackTable* columns_ = nullptr;
    HeaderCover* cover_;
    ui::PlayButton* play_;
    ui::Button* shuffle_;
    collapse::Spec spec_;
    collapse::Frame frame_;
    bool valid_ = false;
    float compactW_ = 0;      // the compact title's layout width
    Rect titleHit_{};         // the title as drawn (page coordinates)
    bool titleHover_ = false;
};

// The cover slot of a page with a collapsing header (its CollapseBar's HeaderCover), nullptr when the page has none
// (other pages, the content still loading, the artist page).
HeaderCover* findHeaderCover(ui::Widget* page);

} // namespace st::app
