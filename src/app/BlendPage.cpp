// Blend: the cover, the cards in "Senin için listeler" and the blend page (Route{RouteKind::SmartList, "blend:<id>"},
// "#play" appended starts it). The store and the exchange are app/Blends.cpp.
#include "app/Blends.h"

#include "app/Blacklist.h"
#include "app/CollapsingHeader.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/SmartListsPage.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "player/Player.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
using catalog::Image;
using catalog::Track;
namespace type = gfx::type;

namespace blends {

namespace {

std::vector<std::vector<Image>> coversOf(const Info& b) {
    std::vector<std::vector<Image>> out;
    std::unordered_set<std::string> seen;
    for (const auto& r : b.mix.rows) {
        const auto* img = catalog::pickImage(r.track.album.images, 300);
        if (!img || img->url.empty() || !seen.insert(img->url).second) continue;
        out.push_back(r.track.album.images);
        if (out.size() == 4) break;
    }
    return out;
}

std::wstring bandLabel(const Info& b) {
    std::wstring s = toUpperTr(tr(L"Blend"));
    if (b.mix.match >= 0 && !b.waiting) s += L" · " + i18n::format(tr(L"%{} uyum"), {std::to_wstring(b.mix.match)});
    return s;
}

} // namespace

void drawCover(gfx::Canvas& c, const Info& b, const Rect& r, float radius) {
    const auto& col = colors();
    const Color tone = tileFill(tileColor("blend:" + b.id));
    const float band = std::round(r.h * 0.3f);
    const Rect art{r.x, r.y, r.w, r.h - band};
    c.pushRoundedClip(r, radius);
    c.fillRect(r, tone);
    const auto covers = coversOf(b);
    if (covers.size() >= 4) {
        const float w = art.w / 2, h = art.h / 2;
        for (size_t i = 0; i < 4; ++i)
            drawArtwork(c, covers[i], {art.x + (i % 2) * w, art.y + (i / 2) * h, w, h}, 0, Placeholder::Album, false, Priority::Low);
    } else if (!covers.empty()) {
        drawArtwork(c, covers[0], art, 0, Placeholder::Album, false, Priority::Low);
    } else {
        const float s = std::round(art.h * 0.34f);
        c.icon("users", art.center(s, s), col.fgSecondary);
    }
    // The band, as the smart lists draw theirs: an accent rule, "BLEND · %87 UYUM" in mono and the names.
    const Rect bb{r.x, r.bottom() - band, r.w, band};
    c.fillRect(bb, tone);
    c.fillRect({bb.x, bb.y, bb.w, std::max(2.f, std::round(r.w / 80))}, accent().base);
    const float pad = std::round(r.w * 0.07f);
    const float labelSize = std::clamp(r.w * 0.05f, 8.f, 11.f);
    const float titleSize = std::clamp(r.w * 0.09f, 12.f, 24.f);
    c.text(bandLabel(b), type::monoLabel.withSize(labelSize), {bb.x + pad, bb.y + band * 0.18f, bb.w - pad * 2, labelSize * 1.5f},
           col.fgSecondary);
    gfx::TextOptions one;
    one.wrap = false;
    one.ellipsis = true;
    gfx::Text title(b.title, type::title.withSize(titleSize), one);
    c.text(title, {bb.x + pad, bb.y + band * 0.18f + labelSize * 1.5f, bb.w - pad * 2, titleSize * 1.3f}, col.fgPrimary);
    c.popLayer();
}

namespace {

SharedCover blendCover(const ui::Widget& owner, const Rect& local, std::shared_ptr<const Info> b) {
    SharedCover s;
    s.rect = owner.toWindow(local);
    s.radius = 2;
    s.owner = &owner;
    s.key = "blend:" + b->id;
    s.paint = [b](gfx::Canvas& c, const Rect& r, float radius) { drawCover(c, *b, r, radius); };
    return s;
}

void openBlend(const std::string& id, bool play) {
    if (ctx().router) ctx().router->navigate({RouteKind::SmartList, "blend:" + id + (play ? "#play" : "")});
}

std::vector<Track> tracksOf(const Info& b) {
    std::vector<Track> out;
    for (const auto& r : b.mix.rows)
        if (!blacklist::isBlocked(r.track)) out.push_back(r.track);
    return out;
}

// A blend's card: the generated cover, the names and "Blend · %87 uyum"; the hover play button like the smart lists'.
class BlendCard : public ui::Widget, public CoverSource {
public:
    explicit BlendCard(Info b) : b_(std::make_shared<const Info>(std::move(b))), title_(b_->title, type::body.withSize(13)),
                                 subtitle_(subtitle(*b_), type::caption) {
        focusable = true;
    }
    bool coverFor(const std::string& key, SharedCover& out) override {
        if (key != "blend:" + b_->id) return false;
        out = blendCover(*this, artRect(), b_);
        return true;
    }
    float preferredHeight(float width) override { return width + 10 + 18 + 4 + 16; }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button == ui::MouseButton::Right) {
            menu(e.windowPos);
            return false;
        }
        if (e.button != ui::MouseButton::Left) return false;
        playPressed_ = canPlay() && playRect().contains(e.pos);
        (playPressed_ ? playPressA_ : press_).to(1, ui::motion::microMs(80));
        invalidate();
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, ui::motion::microMs(160));
        playPressA_.to(0, ui::motion::microMs(160));
        const bool play = playPressed_ && playRect().contains(e.pos);
        playPressed_ = false;
        invalidate();
        if (!rect().contains(e.pos)) return;
        open(play);
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool hot = canPlay() && playRect().contains(e.pos);
        if (hot != playHot_) {
            playHot_ = hot;
            playHotA_.to(hot ? 1.f : 0.f, ui::motion::microMs(ui::motion::fast));
            invalidate();
        }
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override {
        hover_.to(0, ui::motion::microMs(ui::motion::fast));
        playHot_ = false;
        playHotA_.to(0, ui::motion::microMs(ui::motion::fast));
    }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        press_.snap(ui::motion::micro() ? 1.f : 0.f);
        press_.to(0, ui::motion::microMs(160));
        open(false);
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (e.vk != VK_APPS && !(e.vk == VK_F10 && e.shift)) return false;
        const Rect a = toWindow(artRect());
        menu({a.x + 12, a.bottom() - 12});
        return true;
    }
    Rect focusRect() const override { return rect(); }
    void paint(Canvas& c) override {
        const auto& col = colors();
        const float pointer = hover_.value(), p = press_.value();
        const float h = std::max(pointer, keyboardFocused() ? 1.f : 0.f);
        const Rect art = artRect();
        const float shown = pagetx::presence(this);
        c.pushTransform(D2D1::Matrix3x2F::Translation(0, ui::card::lift(pointer, p)));
        c.pushScale(ui::card::pressScale(p), {art.cx(), art.cy()});
        if (shown > 0.f) {
            ui::card::paintBack(c, art, h * shown);
            drawCover(c, *b_, art, gfx::radii().art);
            ui::card::paintEdge(c, art, h);
            if (canPlay())
                ui::card::paintPlay(c, playRect(), {h, playHotA_.value(), playPressA_.value()}, accent().base, accent().hover,
                                    accent().onAccent);
        }
        c.popTransform();
        const Rect tr{rect().x, art.bottom() + 10, rect().w, 18};
        c.text(title_, tr, col.fgPrimary, gfx::VAlign::Center);
        c.text(subtitle_, {rect().x, tr.bottom() + 2, rect().w, 16}, col.fgSecondary, gfx::VAlign::Center);
        c.popTransform();
    }

private:
    bool canPlay() const { return !b_->waiting && !b_->pending && !b_->mix.rows.empty(); }
    Rect artRect() const { return {rect().x, rect().y, rect().w, rect().w}; }
    Rect playRect() const {
        const Rect a = artRect();
        return {a.right() - 12 - 48, a.bottom() - std::round(a.h * 0.3f) - 12 - 48, 48, 48};
    }
    void open(bool play) const {
        const std::string id = b_->id;   // navigating destroys this card
        pagetx::arm(blendCover(*this, artRect(), b_));
        openBlend(id, play && canPlay());
    }
    void menu(gfx::Point at) const {
        const std::string id = b_->id;
        std::vector<ui::MenuItem> items{{tr(L"Aç"), "arrow-up-right", L"", [id] { openBlend(id, false); }}};
        if (canPlay()) {
            items.push_back({tr(L"Çal"), "play", L"", [id] { openBlend(id, true); }});
            const auto tracks = tracksOf(*b_);
            items.push_back({tr(L"Çalma listesi olarak kaydet"), "plus", L"", [tracks] {
                                 promptNewPlaylist([](const std::string& pid) { ctx().router->navigate({RouteKind::Playlist, pid}); }, tracks);
                             }});
        }
        items.push_back({tr(L"Davet bağlantısını kopyala"), "link", L"", [id] { copyInvite(id); }});
        items.push_back(ui::MenuItem::sep());
        ui::MenuItem leaveItem{tr(L"Blend'den ayrıl"), "logout", L"", [id] { leave(id); }};
        leaveItem.destructive = true;
        items.push_back(std::move(leaveItem));
        ui::Menu::open(ctx().window, at, std::move(items));
    }

    std::shared_ptr<const Info> b_;
    gfx::Text title_, subtitle_;
    ui::Anim hover_, press_;
    bool playHot_ = false, playPressed_ = false;
    ui::Anim playHotA_, playPressA_;
};

// "Blend oluştur": an outlined square with a "+", the same size as the cards around it.
class CreateCard : public ui::Widget {
public:
    CreateCard() : title_(tr(L"Blend oluştur"), type::body.withSize(13)), subtitle_(tr(L"Arkadaşınla ortak liste"), type::caption) {
        focusable = true;
    }
    float preferredHeight(float width) override { return width + 10 + 18 + 4 + 16; }
    bool onMouseDown(const ui::MouseEvent& e) override {
        if (e.button != ui::MouseButton::Left) return false;
        press_.to(1, ui::motion::microMs(80));
        return true;
    }
    void onMouseUp(const ui::MouseEvent& e) override {
        press_.to(0, ui::motion::microMs(160));
        if (rect().contains(e.pos)) run();
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::microMs(ui::motion::fast)); }
    void onMouseLeave() override { hover_.to(0, ui::motion::microMs(ui::motion::fast)); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    bool activatable() const override { return true; }
    bool onActivate() override {
        run();
        return true;
    }
    Rect focusRect() const override { return rect(); }
    std::wstring tooltip() const override {
        return tr(L"Senin ve bir arkadaşının dinlediklerinden ortak bir çalma listesi. Arkadaşın davet bağlantısını açınca hazır olur.");
    }
    void paint(Canvas& c) override {
        const auto& col = colors();
        const float h = std::max(hover_.value(), keyboardFocused() ? 1.f : 0.f);
        const Rect art{rect().x, rect().y, rect().w, rect().w};
        c.pushScale(ui::card::pressScale(press_.value()), {art.cx(), art.cy()});
        c.fillRounded(art, gfx::radii().art, col.overlayHover.mulAlpha(h));
        c.strokeRounded(art.inset(0.5f, 0.5f), gfx::radii().art, h > 0.5f ? col.hairStrong : col.hairDefault);
        const float s = std::round(art.w * 0.2f);
        c.icon("plus", art.center(s, s), h > 0.5f ? accent().base : col.fgSecondary);
        c.popTransform();
        const Rect tr{rect().x, art.bottom() + 10, rect().w, 18};
        c.text(title_, tr, col.fgPrimary, gfx::VAlign::Center);
        c.text(subtitle_, {rect().x, tr.bottom() + 2, rect().w, 16}, col.fgSecondary, gfx::VAlign::Center);
    }

private:
    static void run() { Dispatcher::post([] { create(); }); }
    gfx::Text title_, subtitle_;
    ui::Anim hover_, press_;
};

} // namespace

void addCards(ui::Widget* row) {
    for (auto& b : all()) row->add<BlendCard>(std::move(b));
    row->add<CreateCard>();
}

// ---- The page ---------------------------------------------------------------------------------------------------------

namespace {

class BlendHeader : public HeroHeader {
public:
    BlendHeader() {
        play_ = add<ui::PlayButton>(ui::PlayButton::Look::Accent);
        shuffle_ = add<Button>(ButtonKind::Secondary, tr(L"Karıştır"), "shuffle");
        add_ = add<Button>(ButtonKind::IconOutline, L"", "plus");
        more_ = add<Button>(ButtonKind::IconOutline, L"", "more");
        add_->setTooltip(tr(L"Çalma listesine ekle"));
        more_->setTooltip(tr(L"Diğer seçenekler"));
        gfx::TextOptions wrap;
        wrap.wrap = true;
        wrap.maxLines = 3;
        why_ = gfx::Text({}, type::secondary, wrap);
    }
    void setBlend(const Info& b) {
        b_ = std::make_shared<const Info>(b);
        setTitle(b.title);
        why_.setText(whyOf(b));
        int64_t ms = 0;
        for (const auto& r : b.mix.rows) ms += r.track.durationMs;
        totalMs_ = ms;
        const bool ready = !b.waiting && !b.pending && !b.mix.rows.empty();
        play_->setEnabled(ready);
        shuffle_->setEnabled(ready);
        add_->setEnabled(ready);
        const gfx::Hsl h = gfx::toHsl(tileColor("blend:" + b.id));
        setTintColor(gfx::fromHsl({h.h, std::max(h.s, 0.35f), 0.5f}));
        requestLayout();
        invalidate();
    }
    float preferredHeight(float) override { return 232 + 28 + 56 + 24; }
    void layout() override {
        const float y = 232 + 28;
        play_->setRect({0, y, 56, 56});
        const float sw = shuffle_->naturalWidth();
        shuffle_->setRect({56 + 16, y + 8, sw, 40});
        float x = 56 + 16 + sw + 14;
        for (auto* b : {add_, more_}) {
            b->setRect({x, y + 8, 40, 40});
            x += 40 + 14;
        }
        const float tx = kCoverSize + 32, tw = std::max(0.f, rect().w - tx);
        const float th = fitTitle(tw, {88.f, 72.f, 56.f, 44.f, 36.f}, 56.f, type::displayXL);
        whyW_ = std::min(tw, 640.f);
        whyH_ = why_.empty() ? 0.f : std::ceil(why_.measure(whyW_).h);
        float ty = kCoverSize - th - 30 - (why_.empty() ? 0.f : whyH_ + 6);
        labelY_ = ty - 26;
        contentTop_ = labelY_;
        titleBox_ = {tx - titleSize_ * 0.04f, ty, tw, th};
        ty += th + 10;
        whyY_ = ty;
        if (!why_.empty()) ty += whyH_ + 6;
        metaY_ = ty;
    }
    void drawCover(Canvas& c, const Rect& r, float radius) override {
        if (b_) blends::drawCover(c, *b_, r, radius);
    }
    SharedCover sharedCover(const ui::Widget& owner, const Rect& local) override {
        return b_ ? blendCover(owner, local, b_) : SharedCover{};
    }

    Button *add_, *more_;

protected:
    void paintContent(Canvas& c) override {
        if (!b_) return;
        const Rect r = rect();
        const auto& col = colors();
        const float tx = r.x + kCoverSize + 32, tw = std::max(0.f, r.right() - tx);
        c.text(toUpperTr(tr(L"Senin için")) + L" · " + bandLabel(*b_), type::monoLabel, {tx, r.y + labelY_, tw, 14}, col.fgSecondary);
        if (!why_.empty()) c.text(why_, {tx, r.y + whyY_, whyW_, whyH_}, col.fgSecondary);
        std::wstring meta = L"ShadeTube · " + i18n::plural(L"{} şarkı", static_cast<int64_t>(b_->mix.rows.size()));
        if (totalMs_ > 0) meta += L" · " + totalDuration(totalMs_);
        if (b_->connected) meta += L" · " + std::wstring(tr(L"bağlı"));
        c.text(meta, type::secondary, {tx, r.y + metaY_ + 2, tw, 20}, col.fgSecondary);
    }

private:
    static std::wstring whyOf(const Info& b) {
        if (b.pending)
            return tr(L"Bir arkadaşın seni bu Blend'e davet etti. Zevkini paylaşınca ikinizin dinlediklerinden ortak bir liste oluşur.");
        if (b.waiting)
            return tr(L"Davet bağlantısını bir arkadaşına gönder. O da ShadeTube'da açınca ikinizin son aylarda en çok dinlediklerinden "
                      L"ortak bir liste oluşur.");
        std::wstring s = b.mix.shared > 0
                             ? i18n::plural(L"Önce birlikte sevdiğiniz {} şarkı, sonra sırayla her birinizden birer şarkı.", b.mix.shared)
                             : std::wstring(tr(L"Ortak şarkınız henüz yok: sırayla her birinizden birer şarkı."));
        for (const auto& n : b.leftNames) s += L" " + i18n::format(tr(L"{} Blend'den ayrıldı."), {n});
        return s;
    }

    std::shared_ptr<const Info> b_;
    gfx::Text why_;
    float labelY_ = 0, whyY_ = 0, whyW_ = 0, whyH_ = 0, metaY_ = 0;
    int64_t totalMs_ = 0;
};

class BlendPage : public ScrollPage {
public:
    BlendPage(std::string id, bool autoplay) : id_(std::move(id)), autoplay_(autoplay) {
        enableCollapse();
        subscribe(life_.ref(), [this] { refresh(); });
        watch(id_);
        lastWatch_ = steadyMs();
        refresh();
    }
    bool coverSlot(SharedCover& out) override {
        if (header_) return headerCoverSlot(out);
        return loadingCoverSlot(out);
    }
    void paint(Canvas& c) override {
        ScrollPage::paint(c);
        if (steadyMs() - lastWatch_ > 60'000) {   // keep talking to the broker while the page is looked at
            lastWatch_ = steadyMs();
            watch(id_);
        }
    }

private:
    std::string contextUri() const { return "smart:blend:" + id_; }
    bool isPlayingThis() const {
        auto* p = ctx().player;
        return p && p->isPlaying() && p->context().uri == contextUri();
    }

    // A profile arrived / the connection changed: rebuild when the songs or the state changed, else repaint the header.
    void refresh() {
        const auto b = find(id_);
        if (!b && !loaded()) {   // blends.dat is still being read
            if (!header_) showSkeleton(8);
            return;
        }
        if (!b) {
            header_ = nullptr;
            table_ = nullptr;
            auto* c = resetContent();
            c->add<MessagePanel>("users", tr(L"Bu Blend artık yok"), tr(L"Blend'den ayrıldın ya da bu bilgisayardan silindi."),
                                 tr(L"Kitaplığa dön"), [] { ctx().router->navigate({RouteKind::Library}); });
            contentReady();
            return;
        }
        std::string sig = std::to_string(b->waiting) + std::to_string(b->pending) + "|";
        for (const auto& r : b->mix.rows) sig += r.track.id + ";";
        if (header_ && sig == sig_) {
            info_ = *b;
            header_->setBlend(*b);
            return;
        }
        sig_ = std::move(sig);
        info_ = *b;
        const float y = scroll_->scrollY();
        build();
        if (y > 0) {
            scroll_->layout();
            scroll_->scrollTo(y, false);
        }
    }

    void build() {
        auto* c = resetContent(28.f);
        header_ = c->add<BlendHeader>();
        header_->setBlend(info_);
        table_ = nullptr;
        const std::string id = id_;
        header_->more_->onClick = [this, id] {
            const Rect br = header_->more_->toWindow(header_->more_->rect());
            std::vector<ui::MenuItem> items;
            const bool ready = !info_.waiting && !info_.pending && !info_.mix.rows.empty();
            if (!info_.pending) items.push_back({tr(L"Yenile"), "refresh", L"", [id] { blends::refresh(id); }});
            items.push_back({tr(L"Davet bağlantısını kopyala"), "link", L"", [id] { copyInvite(id); }});
            if (!info_.pending) items.push_back({tr(L"Paylaştıklarını gör"), "info", L"", [id] { showShared(id); }});
            if (ready) {
                const auto tracks = tracksOf(info_);
                items.push_back({tr(L"Tümünü sıraya ekle"), "queue", L"", [tracks] {
                                     ctx().player->enqueue(tracks);
                                     toast(tr(L"Sıraya eklendi"));
                                 }});
                items.push_back({tr(L"Çalma listesi olarak kaydet"), "plus", L"", [tracks] {
                                     promptNewPlaylist([](const std::string& pid) { ctx().router->navigate({RouteKind::Playlist, pid}); },
                                                       tracks);
                                 }});
            }
            items.push_back(ui::MenuItem::sep());
            ui::MenuItem leaveItem{tr(L"Blend'den ayrıl"), "logout", L"", [id] { leave(id); }};
            leaveItem.destructive = true;
            items.push_back(std::move(leaveItem));
            ui::Menu::open(ctx().window, {br.x, br.bottom() + 6}, std::move(items));
        };
        if (info_.pending) {
            c->add<MessagePanel>("users", tr(L"Katılmak için zevkini paylaş"),
                                 tr(L"Neyin paylaşılacağını görüp onayladığında ortak liste oluşur. Paylaşılanlar yalnızca bu Blend'in "
                                    L"üyelerine görünür."),
                                 tr(L"Paylaşılacakları gör"), [id] { share(id); });
            attachHeader(header_, nullptr);
        } else if (info_.waiting) {
            c->add<MessagePanel>("link", tr(L"Arkadaşının katılması bekleniyor"),
                                 tr(L"Davet bağlantısını gönder; arkadaşın açıp zevkini paylaşınca ortak listeniz burada belirir. Aynı "
                                    L"anda çevrimiçi olmanız gerekmez."),
                                 tr(L"Bağlantıyı kopyala"), [id] { copyInvite(id); });
            attachHeader(header_, nullptr);
        } else {
            TrackTable::Options opts;
            opts.showAdded = false;
            table_ = c->add<TrackTable>(opts);
            attachHeader(header_, table_, table_);
            // Whose taste each row is.
            labels_.clear();
            for (const auto& r : info_.mix.rows) labels_[r.track.id] = tasteLabel(info_, r);
            table_->chipFor = [this](const Track& t) {
                const auto it = labels_.find(t.id);
                return it == labels_.end() ? std::wstring() : it->second;
            };
            tracks_.clear();
            for (const auto& r : info_.mix.rows) tracks_.push_back(r.track);
            table_->onPlay = [this](int i) { playFrom(i, false); };
            enrichTrackArtwork(tracks_, 24, life_.ref(), [this] { artLanded(); });
            table_->setTracks(tracks_);
        }
        header_->isPlaying = [this] { return isPlayingThis(); };
        header_->play_->onClick = [this] { playFrom(-1, false); };
        header_->shuffle_->onClick = [this] { playFrom(-1, true); };
        header_->add_->onClick = [this] {
            if (!table_) return;
            const Rect br = header_->add_->toWindow(header_->add_->rect());
            showAddToPlaylistMenu(table_->displayedTracks(), {br.x, br.bottom() + 6});
        };
        scroll_->contentChanged();
        contentReady();
        if (autoplay_ && table_) {
            autoplay_ = false;
            playFrom(-1, false);
        }
    }

    void artLanded() {
        if (artPosted_ || !table_) return;
        artPosted_ = true;
        Dispatcher::post([this, ref = life_.ref()] {
            if (ref.expired()) return;
            artPosted_ = false;
            if (!table_) return;
            enrichTrackArtwork(tracks_, 0, life_.ref(), {});
            table_->setTracks(tracks_);
            invalidate();
        });
    }

    void playFrom(int displayIndex, bool shuffle) {
        auto* p = ctx().player;
        if (!p || !table_) return;
        if (displayIndex < 0 && isPlayingThis()) {
            p->togglePause();
            return;
        }
        if (displayIndex < 0 && p->context().uri == contextUri() && p->status() == player::Status::Paused) {
            p->play();
            return;
        }
        auto tracks = table_->displayedTracks();
        if (tracks.empty()) return;
        if (shuffle) p->setShuffle(true);
        p->playContext(std::move(tracks), displayIndex, {contextUri(), info_.title});
    }

    std::string id_;
    bool autoplay_;
    Info info_;
    std::string sig_;
    BlendHeader* header_ = nullptr;
    TrackTable* table_ = nullptr;
    std::vector<Track> tracks_;
    std::unordered_map<std::string, std::wstring> labels_;
    bool artPosted_ = false;
    int64_t lastWatch_ = 0;
};

} // namespace

} // namespace blends

std::unique_ptr<Page> makeBlendPage(const std::string& rawId) {
    std::string id = rawId;
    bool autoplay = false;
    if (const auto hash = id.find("#play"); hash != std::string::npos) {
        id.resize(hash);
        autoplay = true;
    }
    return std::make_unique<blends::BlendPage>(id, autoplay);
}

} // namespace st::app
