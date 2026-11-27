// The artist page (docs/DEVELOPMENT.md, "Artist and Search pages"): a banner hero that collapses into the bar (app/
// CollapsingHeader), the popular songs beside the artist's pick and newest release, the discography with its groups
// as chips (a shelf, or every release as a grid with a sort order), fans also like, playlists featuring the artist and
// an About block. Everything comes from source::artistPage (Spotify, MusicBrainz or any other catalog): a part whose
// data the source doesn't give is left out, never shown empty.
#include "app/Blacklist.h"
#include "app/BrowseModel.h"
#include "app/BrowseWidgets.h"
#include "app/CollapsingHeader.h"
#include "app/HomeWidgets.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Radio.h"
#include "app/Social.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using namespace catalog;
namespace type = gfx::type;
namespace metrics = gfx::metrics;

namespace {

float rowH() { return TrackTable::rowMetrics().h; }

// "26 Haz 2026" / "Haz 2026" / "2026" from an ISO date of any precision.
std::wstring dateText(const std::string& iso) {
    int y = 0, m = 0, d = 0;
    if (iso.size() >= 4) y = std::atoi(iso.substr(0, 4).c_str());
    if (iso.size() >= 7) m = std::atoi(iso.substr(5, 2).c_str());
    if (iso.size() >= 10) d = std::atoi(iso.substr(8, 2).c_str());
    if (y <= 0) return {};
    std::wstring s = std::to_wstring(y);
    if (m >= 1 && m <= 12) s = i18n::monthAbbrev(m) + L" " + s;
    if (d >= 1 && m >= 1 && m <= 12) s = std::to_wstring(d) + L" " + s;
    return s;
}

// Released in the last 30 days (a full date only).
bool isFresh(const std::string& iso) {
    if (iso.size() < 10) return false;
    using namespace std::chrono;
    const int y = std::atoi(iso.substr(0, 4).c_str());
    const unsigned m = static_cast<unsigned>(std::atoi(iso.substr(5, 2).c_str()));
    const unsigned d = static_cast<unsigned>(std::atoi(iso.substr(8, 2).c_str()));
    const year_month_day ymd{year{y}, month{m}, day{d}};
    if (!ymd.ok()) return false;
    const auto age = floor<days>(system_clock::now()) - sys_days{ymd};
    return age.count() >= 0 && age.count() <= 30;
}

// Dev (SHADETUBE_SPOTIFY_DEMO, app/Social.h): artist.json in that folder (a raw queryArtistOverview answer) stands in for
// any Spotify artist while logged out, so the Spotify layout can be checked without a session. Worker thread.
std::optional<ArtistPage> demoArtist(const std::filesystem::path& dir) {
    std::ifstream f(dir / L"artist.json", std::ios::binary);
    if (!f) return std::nullopt;
    const auto j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    return spotify::Api::parseArtistOverview(j.contains("data") ? j["data"] : j);
}

void openUrl(const std::string& url) {
    ShellExecuteW(nullptr, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void playArtist(std::vector<Track> top, const std::string& uri, const std::wstring& name, bool shuffle) {
    auto* p = ctx().player;
    if (!p) return;
    if (top.empty()) return toast(tr(L"Bu sanatçı için popüler şarkı verisi yok"));
    if (shuffle) p->setShuffle(true);
    p->playContext(std::move(top), -1, {uri, name});   // no row picked: the player skips kara liste tracks
}

// =====================================================================================================================
// Hero: the banner (a wide header image, else the photo at the right, else the accent rings), what the artist is,
// the name, the numbers and the actions: play, shuffle, follow, radio, more (queue, link, block); "Engelli" while
// the artist is on the kara liste (a click lifts it).

class ArtistHero : public HeroHeader {
public:
    explicit ArtistHero(const ArtistPage& page) : artist_(page.artist), banner_(page.banner), verified_(page.verified) {
        setTitleStyle(type::hero, false);
        setTitle(toWide(artist_.name));
        images_ = artist_.images;
        placeholder_ = Placeholder::Artist;
        setTintImages(!artist_.images.empty() ? artist_.images : banner_);   // the bar's tint: the photo's color
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        shuffle_ = add<Button>(ButtonKind::IconOutline, L"", "shuffle-thin");
        shuffle_->setTooltip(tr(L"Karıştır"));
        follow_ = add<Button>(ButtonKind::Secondary, tr(L"Takip et"));
        // Spotify's artist radio (needs a Spotify session and a Spotify artist; MusicBrainz artists have none).
        radio_ = add<Button>(ButtonKind::IconOutline, L"", "radio");
        radio_->setTooltip(tr(L"Bu sanatçıya benzer şarkılarla radyo başlat"));
        more_ = add<Button>(ButtonKind::IconOutline, L"", "more");
        more_->setTooltip(tr(L"Diğer seçenekler"));
        blockBtn_ = add<Button>(ButtonKind::Secondary, tr(L"Engelli"), "error");
        blockBtn_->setTooltip(tr(L"Sanatçının engelini kaldır"));
        // The numbers: Spotify's listeners and followers, else ListenBrainz' listens, else MusicBrainz' note.
        std::wstring meta;
        auto add = [&meta](const std::wstring& part) {
            if (part.empty()) return;
            if (!meta.empty()) meta += L" · ";
            meta += part;
        };
        if (page.monthlyListeners > 0)
            add(toUpperTr(i18n::plural(L"{} aylık dinleyici", page.monthlyListeners, {compactCount(page.monthlyListeners)})));
        if (page.followers > 0) add(toUpperTr(i18n::plural(L"{} takipçi", page.followers, {compactCount(page.followers)})));
        if (meta.empty() && artist_.listenCount > 0)
            meta = i18n::plural(L"{} DİNLENME · LISTENBRAINZ", artist_.listenCount, {compactCount(artist_.listenCount)});
        if (meta.empty() && !artist_.disambiguation.empty()) meta = toUpperData(toWide(artist_.disambiguation));
        meta_ = gfx::Text(meta, type::monoMeta);
        sync();
    }
    void sync() {
        const bool f = ctx().library.isFollowing(artist_.id);
        follow_->setLabel(f ? tr(L"Takip ediliyor") : tr(L"Takip et"));
        follow_->setIcon(f ? "check" : "");
        follow_->setActive(f);
        // Spotify's artist radio, or YouTube Music's (the artist's mix).
        radio_->setVisible(radioAvailable() && (artist_.id.rfind("spotify:artist:", 0) == 0 || source::isYtmId(artist_.id)) &&
                           !radioSeed(artist_.id).empty());
        blockBtn_->setVisible(blocked());
        requestLayout();
        invalidate();
    }
    // Kara liste state, re-read whenever the list changed (the track menu on this page, Ayarlar, …).
    bool blocked() {
        if (const uint64_t rev = blacklist::revision(); rev != blockRev_) {
            blockRev_ = rev;
            blocked_ = blacklist::artistMatches({artist_.id, artist_.name});
            if (blockBtn_ && blockBtn_->visible() != blocked_) {   // blocked elsewhere (the track menu, Ayarlar)
                blockBtn_->setVisible(blocked_);
                requestLayout();
            }
        }
        return blocked_;
    }
    float preferredHeight(float) override { return 340; }
    void layout() override {
        const float y = 340 - 56;
        play_->setRect({0, y, 56, 56});
        float x = 56 + 16;
        for (Button* b : {shuffle_, follow_, radio_, more_, blockBtn_}) {
            if (!b->visible()) continue;
            const bool round = b->kind() == ButtonKind::IconOutline;
            const float w = round ? 40.f : b->naturalWidth();
            b->setRect({x, y + 8, w, 40});
            x += w + 12;
        }
        // The name on one line, at the largest size that fits 80 % of the width (drawn by the bar, CollapseBar).
        const float w = rect().w;
        float size = 96;
        for (float s : {96.f, 80.f, 64.f, 52.f, 44.f, 36.f}) {
            size = s;
            titleText().setStyle(type::hero.withSize(s));
            if (titleText().measure().w <= w * 0.8f) break;
        }
        titleSize_ = size;
        titleBox_ = {-size * 0.04f, 92, w, std::ceil(titleText().measure(w).h)};
        contentTop_ = 70;
    }

    // No cover: the banner. The bar gets the photo as a round avatar (the 128 px copy the tint was taken from).
    Rect coverSlot() const override { return {}; }
    void drawAvatar(Canvas& c, const Rect& r) override {
        const auto* img = catalog::pickImage(artist_.images, 300);
        ID2D1Bitmap1* bmp = img ? gfx::ImageCache::get().request(img->url, 128) : nullptr;
        if (bmp) c.imageCircle(bmp, r);
        else drawArtwork(c, {}, r, 0, Placeholder::Artist, true);
    }

    // The banner, behind the content: anchored to the page's top with its bottom edge riding the content up into the
    // bar; the picture moves at half the scroll speed (parallax) and fades into the page before the bar docks, while
    // the photo's color comes in as the bar's tint.
    void paintBackdrop(Canvas& c, const Rect& page, const collapse::Frame& f) override {
        const float bottom = collapse::kBarH + collapse::soft(kBannerH - f.s - collapse::kBarH);
        const float fade = 1.f - collapse::smoothstep(f.s / (kBannerH - collapse::kBarH - 24));
        if (fade > 0.001f) paintBanner(c, {page.x, page.y, page.w, bottom}, -0.5f * f.s, fade);
        paintBarTint(c, page, f);
    }
    void paintBarTint(Canvas& c, const Rect& page, const collapse::Frame& f) override {
        paintTint(c, page, f, collapse::smoothstep(f.titleProgress));
    }

    Button *follow_, *radio_, *more_;
    Button* blockButton() const { return blockBtn_; }   // "Engelli": shown while the artist is blocked

protected:
    void paintContent(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        float x = r.x;
        std::wstring label;
        if (verified_) {   // Spotify's check: a small accent seal, then the words
            c.icon("check", {x, r.y + 70, 14, 14}, accent().base);
            x += 20;
            label = toUpperTr(tr(L"Doğrulanmış sanatçı"));
        } else {
            label = artist_.type == "Group" ? toUpperTr(tr(L"Grup")) : toUpperTr(tr(L"Sanatçı"));
            if (!artist_.country.empty()) label += L" · " + toWide(artist_.country);
            if (!artist_.beginYear.empty()) label += L" · " + toWide(artist_.beginYear);
            if (!artist_.tags.empty()) label += L" · " + toUpperData(toWide(artist_.tags[0]));
        }
        if (blocked()) label.append(L" · ").append(toUpperTr(tr(L"Engelli")));   // kara liste: its songs never play by themselves
        c.text(label, type::monoLabel, {x, r.y + 70, r.right() - x, 14}, accent().base);
        if (!meta_.empty()) c.text(meta_, {r.x, r.y + 100 + titleSize_, r.w, 16}, col.fgSecondary);
    }

private:
    static constexpr float kBannerH = 340 + 36 - 80;   // the banner at rest (it starts 36 above the hero)

    // The picture: a wide header image across the page, else the photo at the banner's right, else the ring motif;
    // `dy` = parallax, `opacity` = how much of it is left. It fades into the page to the left (under the name) and
    // to the bottom (into the content).
    void paintBanner(Canvas& c, const Rect& banner, float dy, float opacity) {
        const auto& col = colors();
        const bool wide = !banner_.empty();
        const bool photo = wide || !artist_.images.empty();
        // A wide image runs under the name: on a narrow page (the name covers more of it) and on paper it fades further.
        const float fadeFrom = wide ? 0.f : 0.4f, fadeTo = wide ? (banner.w < 900 ? 0.9f : 0.66f) : 0.8f;
        const float under = gfx::isLightTheme() ? 0.93f : 0.88f;   // what is left of bg.base under the name
        const Rect pic = wide ? Rect{banner.x, banner.y + dy, banner.w, banner.h + std::max(0.f, -dy)}
                              : Rect{banner.x + banner.w * 0.4f, banner.y - banner.w * 0.08f + dy, banner.w * 0.6f, banner.w * 0.6f};
        if (c.translucent()) {
            // Translucent window (Mica): the picture / rings dissolve by their own opacity (same masks as the bg.base
            // gradients of the opaque path), so the backdrop shows through below them instead of an opaque band.
            c.pushFade(banner, {0, banner.y + banner.h * 0.4f}, {0, banner.bottom()}, 1.f, 0.f);
            c.pushClip(banner);
            if (photo) {
                c.pushFade(banner, {banner.x + banner.w * fadeFrom, 0}, {banner.x + banner.w * fadeTo, 0}, wide ? 1.f - under : 0.f, 0.85f);
                drawPicture(c, pic, opacity);
                c.popLayer();
            } else {
                drawRings(c, banner, dy, opacity);
            }
            c.popClip();
            c.popLayer();
            return;
        }
        c.pushClip(banner);
        if (photo) drawPicture(c, pic, opacity);
        else drawRings(c, banner, dy, opacity);   // no photo: editorial ring motif in the accent
        c.popClip();
        if (photo) {
            // The fade starts exactly at the photo's left edge (0.4 w): starting earlier left that edge ~9 % visible,
            // a hard vertical step wherever the photo differs from bg.base (every dark photo on the light theme).
            // A wide image keeps a little of itself under the name.
            c.fillLinearGradient(banner, {banner.x + banner.w * fadeFrom, 0}, {banner.x + banner.w * fadeTo, 0},
                                 col.bgBase.withAlpha(wide ? under : 1.f), col.bgBase.withAlpha(0.15f));
        }
        c.fillLinearGradient(banner, {0, banner.y + banner.h * 0.4f}, {0, banner.bottom()}, col.bgBase.withAlpha(0), col.bgBase);
    }
    void drawPicture(Canvas& c, const Rect& pic, float opacity) {
        const auto& images = banner_.empty() ? artist_.images : banner_;
        const int px = static_cast<int>(std::ceil(pic.w * c.scale()));
        const auto* img = catalog::pickImage(images, px);
        if (ID2D1Bitmap1* bmp = img ? gfx::ImageCache::get().request(img->url, px) : nullptr) {
            c.image(bmp, pic, 0, opacity);
            return;
        }
        if (!banner_.empty()) return;   // a wide image: nothing while it loads (the page shows through)
        if (opacity < 0.999f) c.pushOpacity(opacity);
        drawArtwork(c, {}, pic, 0, Placeholder::Artist);   // the placeholder while it loads
        if (opacity < 0.999f) c.popLayer();
    }
    void drawRings(Canvas& c, const Rect& banner, float dy, float opacity) {
        for (int i = 0; i < 5; ++i)
            c.strokeCircle({banner.right() - 160, banner.y + kBannerH * 0.5f + 40 + dy}, 220.f - i * 34.f,
                           accent().base.withAlpha((0.10f + i * 0.04f) * opacity), 1.f);
    }

    Artist artist_;
    std::vector<Image> banner_;
    bool verified_ = false;
    gfx::Text meta_;
    Button* blockBtn_ = nullptr;
    bool blocked_ = false;
    uint64_t blockRev_ = ~0ull;
};

// =====================================================================================================================
// A featured release beside the popular songs: the artist's pick (with its comment) or the newest release. A raised
// card tinted with the cover's color: the cover, a mono label, the title, a line, the note; the accent play button
// rises in on hover / focus. Enter opens it, Shift+Enter plays it.

struct Feature {
    std::wstring label;               // "SANATÇI SEÇİMİ", "SON ÇIKAN · 26 HAZ 2026"
    std::wstring title, subtitle, note;
    std::vector<Image> images;
    bool circle = false;
    Placeholder placeholder = Placeholder::Album;
    bool fresh = false;               // out in the last 30 days: a "YENİ" pill
    std::function<void()> open, play;
    std::optional<preview::Target> preview;
};

class FeatureCard : public ui::Widget, public CoverSource, public PreviewSource {
public:
    static constexpr float kH = 148;
    explicit FeatureCard(Feature f)
        : f_(std::move(f)), label_(f_.label, type::monoLabel), title_(f_.title, type::sectionTitle),
          sub_(f_.subtitle, type::secondary), note_(f_.note.empty() ? std::wstring() : L"“" + f_.note + L"”", type::caption),
          fresh_(toUpperTr(tr(L"Yeni")), type::monoBadge) {
        focusable = true;
        gfx::TextOptions two;
        two.wrap = true;
        two.maxLines = 2;
        title_.setOptions(two);
        note_.setOptions(two);
    }
    float preferredHeight(float) override { return kH; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float h = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        c.pushScale(1 - 0.01f * press_.value(), {r.cx(), r.cy()});
        c.fillRounded(r, gfx::radii().surface, col.bgRaised);
        const Rect art = artRect();
        if (!tint_) {
            if (const auto* img = pickImage(f_.images, static_cast<int>(art.w * c.scale())))
                tint_ = gfx::ImageCache::get().accentOf(img->url);
            if (tint_) tintA_.to(1, ui::motion::slow, ui::Ease::Standard);
        }
        if (tint_) {
            const float a = (gfx::isLightTheme() ? 0.20f : 0.30f) * tintA_.value();
            c.fillLinearGradient(r, {r.x, r.y}, {r.x + r.w * 0.9f, r.y + r.h * 0.3f}, tint_->withAlpha(a), tint_->withAlpha(0));
        }
        c.strokeRounded(r.inset(0.5f), gfx::radii().surface, Color::lerp(col.hairDefault, col.hairStrong, h));
        if (pagetx::presence(this) > 0.f) drawArtwork(c, f_.images, art, gfx::radii().art, f_.placeholder, f_.circle);
        const float tx = art.right() + 18, tw = std::max(0.f, r.right() - 16 - tx);
        float y = art.y + 2;
        float lx = tx;
        if (f_.fresh) {
            const float bw = std::ceil(fresh_.measure().w) + 12;
            const Rect pill{lx, y - 1, bw, 16};
            c.fillPill(pill, accent().base);
            c.text(fresh_, pill.inset(6, 0), accent().onAccent, gfx::VAlign::Center);
            lx += bw + 8;
        }
        c.text(label_, {lx, y, std::max(0.f, r.right() - 16 - lx), 14}, col.fgSecondary);
        y += 14 + 8;
        const float th = std::ceil(title_.measure(tw).h);
        c.text(title_, {tx, y, tw, th + 2}, col.fgPrimary);
        y += th + 4;
        if (!sub_.empty()) {
            c.text(sub_, {tx, y, tw, 18}, col.fgSecondary, gfx::VAlign::Center);
            y += 18 + 4;
        }
        const float playLeft = r.right() - 14 - 40 - 8;
        if (!note_.empty() && y < art.bottom() - 8)
            c.text(note_, {tx, y, std::max(0.f, playLeft - tx), art.bottom() - y}, col.fgTertiary);
        if (f_.play)
            ui::card::paintPlay(c, playRect(), {h, playHot_.value(), playPress_.value(), false, 14}, accent().base, accent().hover,
                                accent().onAccent);
        c.popTransform();
    }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        onPlay_ = f_.play && playRect().contains(e.pos);
        (onPlay_ ? playPress_ : press_).to(1, ui::motion::microMs(80));
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, ui::motion::microMs(160));
        playPress_.to(0, ui::motion::microMs(160));
        const bool play = onPlay_;
        onPlay_ = false;
        if (!rect().contains(e.pos)) return;
        run(play && playRect().contains(e.pos) ? f_.play : f_.open);
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = f_.play && playRect().contains(e.pos);
        if (hot == hotOn_) return;
        hotOn_ = hot;
        playHot_.to(hot ? 1.f : 0.f, ui::motion::microMs(ui::motion::fast));
        invalidate();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::microMs(ui::motion::fast));
        hotOn_ = false;
        playHot_.to(0, ui::motion::microMs(ui::motion::fast));
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        run(f_.open);
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_RETURN || !e.shift || e.ctrl || e.alt || !f_.play) return false;
        run(f_.play);
        return true;
    }
    bool coverFor(const std::string& key, SharedCover& out) override {
        if (f_.images.empty()) return false;
        out = cover();
        return out.key == key;
    }
    std::optional<PreviewHit> previewAt(gfx::Point windowPos, bool keyboard) override {
        if (!f_.preview || (!keyboard && !artRect().contains(fromWindow(windowPos)))) return std::nullopt;
        return PreviewHit{*f_.preview, toWindow(artRect())};
    }
    std::wstring tooltip() const override {
        auto& t = const_cast<gfx::Text&>(title_);
        const Rect a = artRect();
        return t.isTrimmed(rect().right() - 16 - (a.right() + 18)) ? f_.title : std::wstring();
    }

private:
    Rect artRect() const {
        const Rect r = rect();
        return {r.x + 16, r.y + 16, r.h - 32, r.h - 32};
    }
    Rect playRect() const {
        const Rect r = rect();
        return {r.right() - 14 - 40, r.bottom() - 14 - 40, 40, 40};
    }
    SharedCover cover() const {
        const Rect a = artRect();
        return pagetx::imageCover(*this, a, f_.circle ? a.w * 0.5f : gfx::radii().art, f_.images);
    }
    void run(const std::function<void()>& fn) {
        if (!fn) return;
        if (!f_.images.empty()) pagetx::arm(cover());
        const auto call = fn;   // may navigate (this card goes away)
        call();
    }
    Feature f_;
    gfx::Text label_, title_, sub_, note_, fresh_;
    std::optional<Color> tint_;
    ui::Anim hover_, press_, playHot_, playPress_, tintA_;
    bool onPlay_ = false, hotOn_ = false;
};

// =====================================================================================================================
// "Popüler": five songs, "Daha fazla göster" opens up to ten (the list grows smoothly; at once with reduce motion),
// beside the featured cards on a wide page (under them on a narrow one).

class PopularBlock : public ui::Widget {
public:
    PopularBlock(std::vector<Track> top, std::wstring meta, std::string ctxUri, std::wstring ctxName, std::vector<Feature> features)
        : top_(std::move(top)), uri_(std::move(ctxUri)), name_(std::move(ctxName)) {
        hitTestVisible = false;
        if (!top_.empty()) {
            header_ = add<SectionHeader>(tr(L"Popüler"), std::move(meta));
            clip_ = add<ui::Box>();
            clip_->clipsChildren = true;
            clip_->hitTestVisible = false;
            TrackTable::Options o;
            o.showAdded = false;
            o.showHeader = false;
            table_ = clip_->add<TrackTable>(o);
            table_->setTracks(slice(collapsedRows()));
            table_->onPlay = [this](int i) {
                auto* p = ctx().player;
                if (p) p->playContext(top_, i, {uri_, name_});
            };
            rows_.snap(static_cast<float>(collapsedRows()));
            if (top_.size() > 5) {
                more_ = add<Button>(ButtonKind::Link, tr(L"Daha fazla göster"));
                more_->onClick = [this] { toggle(); };
            }
        }
        for (auto& f : features) cards_.push_back(add<FeatureCard>(std::move(f)));
    }

    float preferredHeight(float width) override { return place(width, false); }
    void layout() override { place(rect().w, true); }
    void paint(Canvas& c) override {
        if (rows_.running()) {
            requestLayout();   // the list grows / shrinks: the page follows every frame
        } else if (table_ && !expanded_ && table_->tracks().size() > collapsedRows() && !trimPosted_) {
            trimPosted_ = true;   // folded: the hidden rows leave (keyboard, hit tests), after this frame
            Dispatcher::post([this, ref = life_.ref()] {
                if (ref.expired()) return;
                trimPosted_ = false;
                if (!expanded_ && !rows_.running()) table_->setTracks(slice(collapsedRows()));
            });
        }
        paintChildren(c);
    }

private:
    size_t collapsedRows() const { return std::min<size_t>(5, top_.size()); }
    size_t expandedRows() const { return std::min<size_t>(10, top_.size()); }
    std::vector<Track> slice(size_t n) const { return {top_.begin(), top_.begin() + static_cast<std::ptrdiff_t>(n)}; }
    void toggle() {
        expanded_ = !expanded_;
        if (expanded_) table_->setTracks(slice(expandedRows()));
        rows_.to(static_cast<float>(expanded_ ? expandedRows() : collapsedRows()), ui::motion::medium, ui::Ease::Standard);
        more_->setLabel(expanded_ ? tr(L"Daha az göster") : tr(L"Daha fazla göster"));
        requestLayout();
        invalidate();
    }
    float place(float w, bool apply) {
        const bool hasList = table_ != nullptr;
        const bool side = !cards_.empty();
        const bool wide = hasList && side && w >= 900;
        const float sideW = !side ? 0.f : wide ? std::clamp(w * 0.34f, 300.f, 420.f) : std::min(w, 560.f);
        const float listW = wide ? w - sideW - 32 : w;
        float listH = 0;
        if (hasList) {
            const float rows = rows_.value() * rowH();
            const float full = static_cast<float>(table_->tracks().size()) * rowH();
            if (apply) {
                header_->setRect({0, 0, listW, 48});
                clip_->setRect({0, 56, listW, std::ceil(rows)});
                table_->setRect({0, 0, listW, full});
            }
            listH = 56 + std::ceil(rows);
            if (more_) {
                if (apply) more_->setRect({12, listH + 8, more_->naturalWidth(), 32});
                listH += 8 + 32;
            }
        }
        float sideH = 0;
        const float sideX = wide ? listW + 32 : 0.f;
        float y = wide ? 56.f : (hasList ? listH + metrics::sectionGap : 0.f);
        for (auto* card : cards_) {
            if (apply) card->setRect({sideX, y, sideW, FeatureCard::kH});
            y += FeatureCard::kH + metrics::gridGap;
            sideH = y - metrics::gridGap;
        }
        return wide ? std::max(listH, sideH) : std::max(listH, sideH);
    }

    std::vector<Track> top_;
    std::string uri_;
    std::wstring name_;
    SectionHeader* header_ = nullptr;
    ui::Box* clip_ = nullptr;
    TrackTable* table_ = nullptr;
    Button* more_ = nullptr;
    std::vector<FeatureCard*> cards_;
    ui::Anim rows_;
    bool expanded_ = false, trimPosted_ = false;
    Lifetime life_;
};

// A card's place while it loads (the rest of a discography group): the artwork and two lines, shimmering.
class CardSkeleton : public ui::Widget {
public:
    CardSkeleton() { hitTestVisible = false; }
    float preferredHeight(float width) override { return width + 10 + 18 + 4 + 16; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        ui::paintSkeleton(c, this, {r.x, r.y, r.w, r.w}, gfx::radii().art);
        ui::paintSkeleton(c, this, {r.x, r.y + r.w + 14, std::round(r.w * 0.7f), 12}, gfx::radii().badge);
        ui::paintSkeleton(c, this, {r.x, r.y + r.w + 34, std::round(r.w * 0.4f), 10}, gfx::radii().badge);
    }
};

// =====================================================================================================================
// Diskografi: the groups the source has as chips (Popüler sürümler, Albümler, Tekliler ve EP'ler, Derlemeler, Yer
// aldığı). One shelf of the group; "Tümünü gör" shows the whole group as a grid with a sort order (En yeni, En eski,
// Alfabetik; the popular releases also in their own order), "Daha az göster" folds it back. The cards fade in.

class Discography : public ui::Column {
public:
    explicit Discography(const ArtistPage& p) : ui::Column(0) {
        data_.popular = p.popular;
        data_.albums = p.albums;
        data_.singles = p.singles;
        data_.other = p.other;
        data_.appearsOn = p.appearsOn;
        data_.albumsTotal = p.albumsTotal;
        data_.singlesTotal = p.singlesTotal;
        data_.otherTotal = p.otherTotal;
        data_.appearsOnTotal = p.appearsOnTotal;
        artistId_ = p.artist.id;
        owner_ = {p.artist.id, p.artist.name};
        groups_ = browse::groupsOf(data_);
        // Spotify leads with the popular releases; the albums are the default where there is no such list.
        live_ = std::any_of(p.other.begin(), p.other.end(), [](const Album& a) {
            return std::find(a.secondaryTypes.begin(), a.secondaryTypes.end(), "Live") != a.secondaryTypes.end();
        });

        head_ = add<ui::Box>();
        head_->hitTestVisible = false;
        sort_ = head_->add<Button>(ButtonKind::Ghost, L"", "sort");
        sort_->onClick = [this] { showSortMenu(); };
        all_ = head_->add<Button>(ButtonKind::Link, tr(L"Tümünü gör"), "arrow-forward");
        all_->onClick = [this] { post([this] { setGrid(!grid_); }); };
        head_->onPreferredHeight = [](float) { return 48.f; };
        head_->onLayout = [this](ui::Box& b) {
            const float w = b.rect().w;
            const float aw = all_->naturalWidth();
            all_->setRect({w - aw, 0, aw, 36});
            const float sw = sort_->naturalWidth();
            sort_->setRect({w - aw - 16 - sw, 0, sw, 36});
        };
        head_->onPaint = [](ui::Box& b, Canvas& c) {
            const Rect r = b.rect();
            c.text(tr(L"Diskografi"), type::title, {r.x, r.y, r.w * 0.6f, 36}, colors().fgPrimary, gfx::TextAlign::Leading,
                   gfx::VAlign::Center);
            c.hline(r.x, r.right(), r.y + 44, colors().hairDefault);
        };
        std::vector<std::wstring> labels;
        for (auto g : groups_) labels.push_back(label(g));
        chips_ = add<ChipTabs>(std::move(labels), 0);
        chips_->onSelect = [this](int i) {
            post([this, i] {
                group_ = i;
                orderChosen_ = false;
                rebuild();
            });
        };
        setSpacingBefore(chips_, 16);
        body_ = add<FadeColumn>(0.f);
        setSpacingBefore(body_, 4);
        rebuild(false);
    }
    bool empty() const { return groups_.empty(); }

private:
    std::wstring label(browse::Group g) const {
        switch (g) {
        case browse::Group::Popular: return tr(L"Popüler sürümler");
        case browse::Group::Albums: return tr(L"Albümler");
        case browse::Group::Singles: return tr(L"Tekliler ve EP'ler");
        case browse::Group::Compilations: return live_ ? tr(L"Derlemeler ve canlı kayıtlar") : tr(L"Derlemeler");
        case browse::Group::AppearsOn: return tr(L"Yer aldığı");
        }
        return {};
    }
    std::wstring orderLabel() const {
        if (!orderChosen_ && currentGroup() == browse::Group::Popular) return tr(L"Popülerlik");
        switch (order_) {
        case browse::Order::Newest: return tr(L"En yeni");
        case browse::Order::Oldest: return tr(L"En eski");
        case browse::Order::Name: return tr(L"Alfabetik");
        }
        return {};
    }
    browse::Group currentGroup() const { return groups_.empty() ? browse::Group::Albums : groups_[static_cast<size_t>(group_)]; }
    void post(std::function<void()> fn) {
        Dispatcher::post([fn = std::move(fn), ref = life_.ref()] {
            if (!ref.expired()) fn();
        });
    }
    void setGrid(bool on) {
        grid_ = on;
        all_->setLabel(on ? tr(L"Daha az göster") : tr(L"Tümünü gör"));
        all_->setIcon(on ? "" : "arrow-forward");
        rebuild();
        if (!on) head_->scrollIntoView();   // folding a long grid: back to the section
    }
    void showSortMenu() {
        const Rect br = sort_->toWindow(sort_->rect());
        std::vector<ui::MenuItem> items;
        auto add = [&](std::wstring label, browse::Order o, bool source) {
            ui::MenuItem m{std::move(label), "", L"", [this, o, source] {
                               post([this, o, source] {
                                   orderChosen_ = !source;
                                   order_ = o;
                                   rebuild();
                               });
                           }};
            m.checked = source ? !orderChosen_ : orderChosen_ || currentGroup() != browse::Group::Popular ? order_ == o : false;
            items.push_back(std::move(m));
        };
        if (currentGroup() == browse::Group::Popular) add(tr(L"Popülerlik"), order_, true);
        add(tr(L"En yeni"), browse::Order::Newest, false);
        add(tr(L"En eski"), browse::Order::Oldest, false);
        add(tr(L"Alfabetik"), browse::Order::Name, false);
        ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
    }
    void rebuild(bool fade = true) {
        if (groups_.empty()) return;
        const browse::Group g = currentGroup();
        std::vector<Album> list = browse::releasesOf(data_, g);
        const bool keepSourceOrder = g == browse::Group::Popular && !orderChosen_;
        if (grid_ && !keepSourceOrder) browse::sortReleases(list, order_);
        sort_->setLabel(orderLabel());
        const bool partial = total(g) > static_cast<int>(list.size());
        sort_->setVisible(grid_ && list.size() > 1);
        all_->setVisible(grid_ || list.size() > 3 || partial);
        if (grid_ && partial) fetchAll(g);
        head_->requestLayout();
        body_->clearChildren();
        const bool withArtist = g == browse::Group::AppearsOn;
        ui::Widget* parent = grid_ ? static_cast<ui::Widget*>(addCardGrid(body_)) : home::shelfRow(body_);
        if (grid_) body_->setSpacingBefore(parent, 16);
        for (const auto& a : list) addAlbumCard(parent, a, albumSubtitle(a, withArtist));
        if (grid_ && partial)   // the rest is on its way: placeholders where it will be
            for (int i = 0; i < 4; ++i) parent->add<CardSkeleton>();
        if (fade) {
            body_->snapOpacity(0);
            body_->fadeTo(1, ui::motion::microMs(ui::motion::normal));
        }
        requestLayout();
    }

    // The size of a group when its list is only the first part of it (0: the list is all of it).
    int total(browse::Group g) const {
        switch (g) {
        case browse::Group::Albums: return data_.albumsTotal;
        case browse::Group::Singles: return data_.singlesTotal;
        case browse::Group::Compilations: return data_.otherTotal;
        case browse::Group::AppearsOn: return data_.appearsOnTotal;
        default: return 0;
        }
    }
    // The whole group for the grid (source::artistReleases), once; what it gives replaces the first part.
    void fetchAll(browse::Group g) {
        if (std::find(fetching_.begin(), fetching_.end(), g) != fetching_.end()) return;
        fetching_.push_back(g);
        const ReleaseGroup rg = g == browse::Group::Singles        ? ReleaseGroup::Singles
                                : g == browse::Group::Compilations ? ReleaseGroup::Compilations
                                : g == browse::Group::AppearsOn    ? ReleaseGroup::AppearsOn
                                                                   : ReleaseGroup::Albums;
        spotify::Api* api = source::activeApi();
        async(Priority::High, life_.ref(), [api, id = artistId_, rg] { return source::artistReleases(api, id, rg); },
              [this, g](Result<std::vector<Album>> r) {
                  std::vector<Album>& list = g == browse::Group::Singles        ? data_.singles
                                             : g == browse::Group::Compilations ? data_.other
                                             : g == browse::Group::AppearsOn    ? data_.appearsOn
                                                                                : data_.albums;
                  if (r && r->size() > list.size()) {
                      list = std::move(*r);
                      if (g != browse::Group::AppearsOn)   // the artist's own releases: credited to the artist
                          for (auto& a : list)
                              if (a.artists.empty()) a.artists.push_back(owner_);
                  }
                  switch (g) {   // complete now (or as complete as it gets: no second try)
                  case browse::Group::Albums: data_.albumsTotal = 0; break;
                  case browse::Group::Singles: data_.singlesTotal = 0; break;
                  case browse::Group::Compilations: data_.otherTotal = 0; break;
                  case browse::Group::AppearsOn: data_.appearsOnTotal = 0; break;
                  default: break;
                  }
                  if (grid_ && currentGroup() == g) rebuild(false);
              });
    }

    ArtistPage data_;   // the release lists
    std::string artistId_;
    ArtistRef owner_;
    std::vector<browse::Group> fetching_;
    std::vector<browse::Group> groups_;
    int group_ = 0;
    bool grid_ = false, orderChosen_ = false, live_ = false;
    browse::Order order_ = browse::Order::Newest;
    ui::Box* head_;
    Button *sort_, *all_;
    ChipTabs* chips_;
    FadeColumn* body_;
    Lifetime life_;
};

// =====================================================================================================================
// Hakkında: a photo, the numbers (listeners, followers, rank; ListenBrainz listens, founded / born, country), the
// biography (five lines that fade out, "Devamını göster" unrolls the rest), the cities that listen most, the genres
// and the artist's own places as links.

class AboutCard : public ui::Widget {
public:
    explicit AboutCard(const ArtistPage& p) {
        hitTestVisible = false;
        const Artist& a = p.artist;
        // A photo of the gallery (the hero already shows the avatar / banner); none without one.
        if (!p.gallery.empty()) photo_ = {p.gallery.front()};
        auto stat = [this](std::wstring value, std::wstring label) {
            stats_.push_back({gfx::Text(std::move(value), type::displayS.withSize(28)), gfx::Text(toUpperTr(label), type::monoLabel)});
        };
        if (p.monthlyListeners > 0) stat(compactCount(p.monthlyListeners), tr(L"Aylık dinleyici"));
        if (p.followers > 0) stat(compactCount(p.followers), tr(L"Takipçi"));
        if (p.worldRank > 0) stat(L"#" + std::to_wstring(p.worldRank), tr(L"Dünya sıralaması"));
        if (p.monthlyListeners <= 0 && a.listenCount > 0) stat(compactCount(a.listenCount), tr(L"ListenBrainz dinlenmesi"));
        if (!a.beginYear.empty())
            stat(toWide(a.beginYear), a.type == "Group" ? tr(L"Kuruluş") : a.type == "Person" ? tr(L"Doğum yılı") : tr(L"Başlangıç"));
        if (!a.country.empty()) stat(toWide(a.country), tr(L"Ülke"));
        if (!p.bio.empty()) {
            gfx::TextOptions wrap;
            wrap.wrap = true;
            bio_ = gfx::Text(toWide(p.bio), type::bodyL, wrap);
            std::wstring head = toUpperTr(tr(L"Biyografi"));
            if (!p.bioSource.empty()) head += L" · " + toUpperData(toWide(p.bioSource));
            bioHead_ = gfx::Text(head, type::monoLabel);
            more_ = add<Button>(ButtonKind::Link, tr(L"Devamını göster"));
            more_->onClick = [this] {
                open_ = !open_;
                bioA_.to(open_ ? 1.f : 0.f, ui::motion::medium, ui::Ease::Standard);
                more_->setLabel(open_ ? tr(L"Daha az göster") : tr(L"Devamını göster"));
                requestLayout();
                invalidate();
            };
        }
        if (!p.topCities.empty()) {
            citiesHead_ = gfx::Text(toUpperTr(tr(L"En çok dinlendiği şehirler")), type::monoLabel);
            for (size_t i = 0; i < p.topCities.size() && i < 5; ++i) {
                const auto& c = p.topCities[i];
                std::wstring city = toWide(c.city);
                if (!c.country.empty()) city += L", " + toWide(c.country);
                cities_.push_back({gfx::Text(city, type::secondary),
                                   gfx::Text(toUpperTr(i18n::plural(L"{} dinleyici", c.listeners, {compactCount(c.listeners)})),
                                             type::monoMeta)});
            }
        }
        for (size_t i = 0; i < a.tags.size() && i < 6; ++i) tags_.emplace_back(toUpperData(toWide(a.tags[i])), type::monoLabel);
        for (const auto& l : p.links) {
            auto* b = add<Button>(ButtonKind::Secondary, linkLabel(l.kind), "arrow-up-right");
            b->setIconSize(14);
            b->setTooltip(toWide(l.url));
            b->onClick = [url = l.url] { openUrl(url); };
            links_.push_back(b);
        }
    }
    bool empty() const { return stats_.empty() && bio_.empty() && cities_.empty() && tags_.empty() && links_.empty(); }

    float preferredHeight(float width) override { return place(width, false); }
    void layout() override { place(rect().w, true); }
    void paint(Canvas& c) override {
        if (bioA_.running()) requestLayout();
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, gfx::radii().surface, col.bgRaised);
        c.strokeRounded(r.inset(0.5f), gfx::radii().surface, col.hairDefault);
        if (!photoR_.empty()) drawArtwork(c, photo_, photoR_.offset(r.x, r.y), gfx::radii().art, Placeholder::Artist);
        for (size_t i = 0; i < stats_.size(); ++i) {
            const Rect s = statR_[i].offset(r.x, r.y);
            c.text(stats_[i].value, {s.x, s.y, s.w + 40, 32}, col.fgPrimary, gfx::VAlign::Center);
            c.text(stats_[i].label, {s.x, s.y + 38, s.w + 40, 14}, col.fgTertiary);
        }
        if (!bio_.empty()) {
            const Rect b = bioR_.offset(r.x, r.y);
            c.text(bioHead_, {b.x, b.y, b.w, 14}, col.fgTertiary);
            const Rect t{b.x, b.y + 24, b.w, b.h - 24};
            c.pushClip(t);
            c.text(bio_, {t.x, t.y, t.w, bioFullH_}, col.fgSecondary);
            c.popClip();
            // More below: the last line fades out into the card (it is opaque, so a gradient of its fill; no layer).
            if (bioFullH_ > t.h + 1) {
                const Rect f{t.x, t.bottom() - 44, t.w, 44};
                c.fillLinearGradient(f, {0, f.y}, {0, f.bottom()}, col.bgRaised.withAlpha(0), col.bgRaised);
            }
        }
        if (!cities_.empty()) {
            const Rect cr = citiesR_.offset(r.x, r.y);
            c.text(citiesHead_, {cr.x, cr.y, cr.w, 14}, col.fgTertiary);
            const float colW = cr.w >= 520 ? (cr.w - 24) * 0.5f : cr.w;
            const int per = colW < cr.w ? static_cast<int>((cities_.size() + 1) / 2) : static_cast<int>(cities_.size());
            for (size_t i = 0; i < cities_.size(); ++i) {
                const float x = cr.x + (static_cast<int>(i) >= per ? colW + 24 : 0.f);
                const float y = cr.y + 24 + (static_cast<int>(i) % per) * 24.f;
                const float lw = std::ceil(cities_[i].second.measure().w);
                c.text(cities_[i].first, {x, y, colW - lw - 12, 20}, col.fgPrimary, gfx::VAlign::Center);
                c.text(cities_[i].second, {x + colW - lw, y, lw + 1, 20}, col.fgTertiary, gfx::VAlign::Center);
            }
        }
        for (size_t i = 0; i < tags_.size(); ++i) {
            const Rect t = tagR_[i].offset(r.x, r.y);
            c.strokePill(t.inset(0.5f), col.hairStrong);
            c.text(tags_[i], t.inset(10, 0), col.fgSecondary, gfx::VAlign::Center);
        }
        paintChildren(c);
    }

private:
    static std::wstring linkLabel(const std::string& kind) {
        if (kind == "website") return tr(L"Web sitesi");
        if (kind == "wikipedia") return L"Wikipedia";
        if (kind == "instagram") return L"Instagram";
        if (kind == "twitter") return L"X";
        if (kind == "facebook") return L"Facebook";
        if (kind == "youtube") return L"YouTube";
        if (kind == "tiktok") return L"TikTok";
        if (kind == "soundcloud") return L"SoundCloud";
        if (kind == "bandcamp") return L"Bandcamp";
        std::wstring k = toWide(kind);
        if (!k.empty()) k[0] = static_cast<wchar_t>(towupper(k[0]));
        return k;
    }
    // Lays the card out for `w` (local coordinates); returns its height.
    float place(float w, bool apply) {
        const float pad = w >= 600 ? 28.f : 20.f;
        const bool photo = !photo_.empty();
        const bool wide = photo && w >= 760;
        const float photoW = wide ? std::clamp(w * 0.3f, 220.f, 320.f) : 0.f;
        const float x = wide ? pad + photoW + 32 : pad;
        const float cw = std::max(40.f, w - x - pad);
        float y = pad;
        if (photo && !wide) {
            const float ph = std::min(220.f, std::round((w - 2 * pad) * 0.5f));
            if (apply) photoR_ = {pad, y, w - 2 * pad, ph};
            y += ph + 24;
        } else if (wide && apply) {
            photoR_ = {pad, pad, photoW, photoW * 1.2f};
        } else if (apply) {
            photoR_ = {};
        }
        // The numbers, side by side (wrapping).
        if (!stats_.empty()) {
            float sx = 0, sy = 0;
            if (apply) statR_.resize(stats_.size());
            for (size_t i = 0; i < stats_.size(); ++i) {
                const float sw = std::max(std::ceil(stats_[i].value.measure().w), std::ceil(stats_[i].label.measure().w));
                if (sx > 0 && sx + sw > cw) {
                    sx = 0;
                    sy += 52 + 16;
                }
                if (apply) statR_[i] = {x + sx, y + sy, sw, 52};
                sx += sw + 40;
            }
            y += sy + 52 + 28;
        }
        if (!bio_.empty()) {
            bioFullH_ = std::ceil(bio_.measure(cw).h);
            const float lh = type::bodyL.size * type::bodyL.lineHeight;
            const float clamp = std::min(bioFullH_, std::ceil(lh * 5));
            const float shown = clamp + (bioFullH_ - clamp) * bioA_.value();
            const bool more = bioFullH_ > clamp + 1;
            if (apply) bioR_ = {x, y, cw, 24 + shown};
            y += 24 + shown;
            if (more_) {
                more_->setVisible(more);
                if (more) {
                    if (apply) more_->setRect({x, y + 8, more_->naturalWidth(), 28});
                    y += 8 + 28;
                }
            }
            y += 28;
        }
        if (!cities_.empty()) {
            const float colW = cw >= 520 ? (cw - 24) * 0.5f : cw;
            const size_t rows = colW < cw ? (cities_.size() + 1) / 2 : cities_.size();
            if (apply) citiesR_ = {x, y, cw, 24 + rows * 24.f};
            y += 24 + rows * 24.f + 24;
        }
        if (!tags_.empty()) {
            float tx = 0, ty = 0;
            if (apply) tagR_.resize(tags_.size());
            for (size_t i = 0; i < tags_.size(); ++i) {
                const float tw = std::ceil(tags_[i].measure().w) + 20;
                if (tx > 0 && tx + tw > cw) {
                    tx = 0;
                    ty += 26 + 8;
                }
                if (apply) tagR_[i] = {x + tx, y + ty, tw, 26};
                tx += tw + 8;
            }
            y += ty + 26 + 24;
        }
        if (!links_.empty()) {
            float lx = 0, ly = 0;
            for (auto* b : links_) {
                const float bw = b->naturalWidth();
                if (lx > 0 && lx + bw > cw) {
                    lx = 0;
                    ly += 32 + 8;
                }
                if (apply) b->setRect({x + lx, y + ly, bw, 32});
                lx += bw + 8;
            }
            y += ly + 32 + 24;
        }
        y = y - 24 + pad;
        if (wide) y = std::max(y, pad + photoW * 1.2f + pad);
        return std::ceil(y);
    }

    struct Stat {
        gfx::Text value, label;
    };
    std::vector<Image> photo_;
    std::vector<Stat> stats_;
    gfx::Text bioHead_, bio_, citiesHead_;
    std::vector<std::pair<gfx::Text, gfx::Text>> cities_;
    std::vector<gfx::Text> tags_;
    std::vector<Button*> links_;
    Button* more_ = nullptr;
    ui::Anim bioA_;
    bool open_ = false;
    float bioFullH_ = 0;
    Rect photoR_{}, bioR_{}, citiesR_{};
    std::vector<Rect> statR_, tagR_;
};

// =====================================================================================================================
// The page

class ArtistView : public ScrollPage {
public:
    explicit ArtistView(std::string id) : id_(std::move(id)) {
        if (const auto hash = id_.find("#play"); hash != std::string::npos) {   // a card's / the top result's play
            id_.resize(hash);
            autoplay_ = true;
        }
        enableCollapse();   // the hero collapses into a compact bar (app/CollapsingHeader)
        showSkeleton(5);
        load();
    }

private:
    void load() {
        hero_ = nullptr;   // the skeleton / error panel replaced it
        const std::string id = id_;
        spotify::Api* api = source::activeApi();
        const std::filesystem::path demo = !api && source::isSpotifyId(id) ? socialDemoDir() : std::filesystem::path();
        async(Priority::High, life_.ref(),
              [id, api, demo] {
                  if (!demo.empty())
                      if (auto d = demoArtist(demo)) return *d;
                  return source::artistPage(api, id);
              },
              [this](Result<catalog::ArtistPage> r) {
                  if (!r) {
                      showError(toWide(r.errorMessage()), [this] {
                          showSkeleton(5);
                          load();
                      });
                      return;
                  }
                  build(*r);
              });
    }

    // The featured cards: the pick, then the newest release (unless the pick is that release).
    std::vector<Feature> features(const ArtistPage& d) {
        std::vector<Feature> out;
        const std::wstring artistName = toWide(d.artist.name);
        if (!d.pick.item.id.empty()) {
            const CardItem& it = d.pick.item;
            Feature f;
            f.label = toUpperTr(tr(L"Sanatçı seçimi"));
            f.title = toWide(it.title);
            f.subtitle = toWide(it.subtitle);
            f.note = toWide(d.pick.comment);
            f.images = it.images;
            const std::string id = it.id;
            switch (it.kind) {
            case CardItem::Kind::Album:
                f.open = [id] { ctx().router->navigate({RouteKind::Album, id}); };
                f.play = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
                f.preview = preview::collectionTarget({RouteKind::Album, id}, f.title, f.images);
                break;
            case CardItem::Kind::Playlist:
                f.placeholder = Placeholder::Playlist;
                f.open = [id] { ctx().router->navigate({RouteKind::Playlist, id}); };
                f.play = [id] { ctx().router->navigate({RouteKind::Playlist, id + "#play"}); };
                f.preview = preview::collectionTarget({RouteKind::Playlist, id}, f.title, f.images);
                break;
            case CardItem::Kind::Artist:
                f.circle = true;
                f.placeholder = Placeholder::Artist;
                f.open = [id] { ctx().router->navigate({RouteKind::Artist, id}); };
                break;
            case CardItem::Kind::Track:
                f.open = f.play = [t = it.track, artistName] {
                    if (auto* p = ctx().player) p->playContext({t}, 0, {"", artistName});
                };
                break;
            }
            out.push_back(std::move(f));
        }
        if (const auto latest = browse::newestRelease(d); latest && latest->id != d.pick.item.id) {
            Feature f;
            f.fresh = isFresh(latest->firstReleaseDate);
            std::wstring label = toUpperTr(tr(L"Son çıkan"));
            if (const std::wstring date = dateText(latest->firstReleaseDate); !date.empty()) label += L" · " + toUpperTr(date);
            f.label = label;
            f.title = toWide(latest->name);
            std::wstring sub = releaseKindLabel(latest->primaryType);
            if (latest->totalTracks > 0) sub += L" · " + i18n::plural(L"{} şarkı", latest->totalTracks);
            f.subtitle = sub;
            f.images = latest->images;
            const std::string id = latest->id;
            f.open = [id] { ctx().router->navigate({RouteKind::Album, id}); };
            f.play = [id] { ctx().router->navigate({RouteKind::Album, id + "#play"}); };
            f.preview = preview::collectionTarget({RouteKind::Album, id}, f.title, f.images);
            out.push_back(std::move(f));
        }
        return out;
    }

    void build(const catalog::ArtistPage& d) {
        auto* c = resetContent(metrics::sectionGap + 8);
        auto* hero = c->add<ArtistHero>(d);
        hero_ = hero;
        const std::string ctxUri = "artist:" + id_;
        const std::wstring ctxName = toWide(d.artist.name);
        const auto top = d.topTracks;
        const Artist artist = d.artist;
        attachHeader(hero, nullptr);
        hero->isPlaying = [ctxUri] {
            auto* p = ctx().player;
            return p && p->isPlaying() && p->context().uri == ctxUri;
        };
        hero->play_->onClick = [top, ctxUri, ctxName] {
            auto* p = ctx().player;
            if (p && p->context().uri == ctxUri) return p->togglePause();
            playArtist(top, ctxUri, ctxName, false);
        };
        hero->shuffle_->onClick = [top, ctxUri, ctxName] { playArtist(top, ctxUri, ctxName, true); };
        hero->shuffle_->setVisible(top.size() > 1);
        hero->follow_->onClick = [artist, hero] {
            ctx().library.setFollow(artist, !ctx().library.isFollowing(artist.id));
            hero->sync();
        };
        hero->radio_->onClick = [artist] { startRadio(artist.id, toWide(artist.name)); };
        auto setBlocked = [this, artist](bool block) {
            blacklist::setArtistBlocked(artist.id, artist.name, block);
            const std::wstring name = toWide(artist.name);
            toast(i18n::format(block ? tr(L"\"{}\" engellendi. Şarkıları artık kendiliğinden çalmaz.")
                                     : tr(L"\"{}\" engeli kaldırıldı"),
                               {name}));
            if (hero_) hero_->sync();
        };
        if (auto* b = hero->blockButton()) b->onClick = [setBlocked] { setBlocked(false); };
        hero->more_->onClick = [this, hero, top, artist, setBlocked] {
            const Rect br = hero->more_->toWindow(hero->more_->rect());
            std::vector<ui::MenuItem> items{
                {tr(L"Popüler şarkıları sıraya ekle"), "queue", L"", [top] {
                     ctx().player->enqueue(top);
                     toast(tr(L"Sıraya eklendi"));
                 }},
                {source::copyLinkLabel(artist.id), "link", L"", [artist] {
                     copyText(source::webLink("artist", artist.id));
                     toast(tr(L"Bağlantı kopyalandı"));
                 }}};
            if (top.empty()) items.erase(items.begin());
            // Kara liste (same wording as the track menu): its songs are skipped wherever the player picks by itself.
            const bool blocked = hero->blocked();
            items.push_back(ui::MenuItem::sep());
            items.push_back({blocked ? tr(L"Sanatçının engelini kaldır") : tr(L"Sanatçıyı engelle"),
                             blocked ? "check" : "error", L"",
                             [setBlocked, blocked, ref = life_.ref()] {
                                 if (!ref.expired()) setBlocked(!blocked);
                             }});
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        hero->sync();

        // Popular songs + the featured releases.
        auto feats = features(d);
        if (!top.empty() || !feats.empty()) {
            // MusicBrainz artists' popular songs and similar artists come from ListenBrainz: it says so.
            const std::wstring meta = browse::isMbid(d.artist.id) ? std::wstring(L"LISTENBRAINZ") : std::wstring();
            c->add<PopularBlock>(top, meta, ctxUri, ctxName, std::move(feats));
        }
        // Discography.
        if (auto* disc = c->add<Discography>(d); disc->empty()) c->removeChild(disc);
        // Fans also like.
        if (!d.similar.empty()) {
            auto* row = home::addShelf(c, tr(L"Hayranları bunları da dinliyor"),
                                       browse::isMbid(d.artist.id) ? std::wstring(L"LISTENBRAINZ") : std::wstring());
            for (const auto& a : d.similar) addArtistCard(row, a);
        }
        // Playlists featuring the artist.
        if (!d.playlists.empty()) {
            auto* row = home::addShelf(c, tr(L"Öne çıktığı çalma listeleri"));
            for (const auto& p : d.playlists) addPlaylistCard(row, p);
        }
        // About.
        if (auto about = std::make_unique<AboutCard>(d); !about->empty()) {
            c->add<SectionHeader>(tr(L"Hakkında"));
            auto* card = static_cast<AboutCard*>(c->adopt(std::move(about)));
            c->setSpacingBefore(card, 16);
        }
        contentReady();
        if (autoplay_) {
            autoplay_ = false;
            playArtist(top, ctxUri, ctxName, false);
        }
    }

    std::string id_;
    bool autoplay_ = false;
    ArtistHero* hero_ = nullptr;   // the current content's header (content is rebuilt only by build())
};

} // namespace

std::unique_ptr<Page> makeArtistPage(const std::string& id) { return std::make_unique<ArtistView>(id); }

} // namespace st::app
