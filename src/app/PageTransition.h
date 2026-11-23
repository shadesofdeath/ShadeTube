#pragma once
// Page transitions with a shared-element cover (docs/DEVELOPMENT.md, "Page transitions").
//
// PageHost (app/Shell) keeps the page it replaces for the ~0.2 s of the change, detached from the tree (no input, no
// focus, no layout), and draws both through PageTransition: the outgoing one fading / shifting out, the incoming one
// fading / sliding in (ui/PageMotion), Back mirrored. When a card or row with a cover opens a page with a cover in its
// header, that cover flies from the card to the header (and back to the card on Back, when it is still there); the two
// owners skip their own artwork meanwhile (pagetx::presence). No cover to fly to, an image that is not loaded, or a page
// rebuilt into something else: the plain transition. Reduce motion, the setting or Windows' "Animation effects" off:
// pages change at once.
#include "app/Router.h"
#include "catalog/Models.h"
#include "ui/PageMotion.h"
#include "ui/Widget.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::app {

using gfx::Rect;

// A cover as one page shows it, for the flight.
struct SharedCover {
    Rect rect;                                   // window DIPs
    float radius = 2;                            // corner radius (round: rect.w / 2)
    std::vector<catalog::Image> images;          // the artwork (drawn like drawArtwork)
    // Artwork that is not one image (the Liked Songs tile, a smart list's generated cover): drawn by this instead. It
    // must capture by value: it is called after the widget may be gone.
    std::function<void(gfx::Canvas&, const Rect&, float radius)> paint;
    std::string key;                             // what it shows ("" = nothing to recognize it by on another page)
    const void* owner = nullptr;                 // the widget painting it: it skips the cover while the cover flies
    bool ready = true;                           // the owner paints the real artwork now (its image has loaded)
};

// A widget showing covers that open pages (cards, rows, the Library grid, the sidebar rail): finding the one to fly
// back to.
class CoverSource {
public:
    // The cover this widget shows with `key` (SharedCover::key), in window DIPs; false when it shows none.
    virtual bool coverFor(const std::string& key, SharedCover& out) = 0;

protected:
    ~CoverSource() = default;
};

namespace pagetx {

// Transitions are on: Ayarlar › Görünüm › Sayfa geçişleri, not "Hareketi azalt", and Windows' animation effects
// (SPI_GETCLIENTAREAANIMATION, read per page change).
bool enabled();
// A card / row about to open a page from its cover (right before it navigates): the next page change flies it.
void arm(SharedCover cover);
// How much `owner` paints its own cover now: 1 normally; 0 while the flight shows it (skip the artwork, its placeholder
// and its shadow); between 0 and 1 for the few frames it lands (draw it; shadows at that strength).
float presence(const void* owner);
// A cover is flying into the page being opened (its loading skeleton leaves its cover block out).
bool coverIncoming();

// The SharedCover of artwork `owner` draws with drawArtwork(images, local, radius): window rect, key = the first
// image's URL, ready = the image is loaded at the size it is drawn.
SharedCover imageCover(const ui::Widget& owner, const Rect& local, float radius, const std::vector<catalog::Image>& images);
// The cover the Liked Songs page and the Library's Liked Songs cell draw without a picture: the accent tile with the
// heart (key "liked").
SharedCover likedCover(const ui::Widget& owner, const Rect& local, float radius);
void drawLikedTile(gfx::Canvas& c, const Rect& r, float radius);

} // namespace pagetx

class PageTransition {
public:
    PageTransition();
    ~PageTransition();
    PageTransition(const PageTransition&) = delete;
    PageTransition& operator=(const PageTransition&) = delete;

    // The host switched pages: `incoming` is already its child; `outgoing` (null for the first page) still is, and is
    // taken out of the tree here (its focus is forgotten as on any page change) and kept until it has faded out.
    void begin(ui::Widget& host, Page* outgoing, Page* incoming, const Router::Change& change);
    // Paints the host's content for this frame (the plain children when nothing moves).
    void paint(gfx::Canvas& c, ui::Widget& host, Page* incoming);

    float presenceOf(const void* owner) const;
    bool coverIncoming() const;

private:
    enum class Target { None, Slot, Match };   // the flight lands on the incoming page's header / a card on it
    bool resolve(ui::Widget& host, Page* incoming, SharedCover& out) const;
    void dropOutgoing(ui::Widget& host);
    void drawFlight(gfx::Canvas& c, ui::Widget& host);

    ui::PageMotion motion_;
    ui::CoverFlight flight_;
    ui::PageNav nav_ = ui::PageNav::Replace;
    double start_ = 0, scale_ = 1;
    std::unique_ptr<ui::Widget> out_;
    float outAlpha_ = 1;            // the outgoing page's opacity when it started leaving (an interrupted arrival)
    SharedCover src_, dst_;
    Target target_ = Target::None;
    std::string matchKey_;
    Rect matchNear_{}, hostWin_{};
    bool firstFrame_ = false;
};

} // namespace st::app
