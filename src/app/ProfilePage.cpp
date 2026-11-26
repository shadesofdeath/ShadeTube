// Profile (Route{RouteKind::Profile, "spotify:user:<name>"}): a Spotify user found by search or a pasted link, with
// their public playlists (spotify/SpotifyApi profile() + profilePlaylists(), paged with "Daha fazla yükle").
#include "app/BrowseWidgets.h"
#include "app/PageWidgets.h"
#include "app/Pages.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/SpotifyApi.h"
#include "ui/Window.h"

#include <algorithm>

namespace st::app {

using gfx::colors;
using namespace catalog;
namespace type = gfx::type;

namespace {

class ProfileHeader : public ui::Widget {
public:
    explicit ProfileHeader(const Profile& p) : profile_(p) {
        hitTestVisible = false;
        std::wstring meta;
        auto add = [&](const std::wstring& part) { meta += (meta.empty() ? L"" : L" · ") + part; };
        if (p.followers >= 0) add(i18n::plural(L"{} takipçi", static_cast<long long>(p.followers)));
        if (p.publicPlaylists > 0) add(i18n::plural(L"{} herkese açık çalma listesi", p.publicPlaylists));
        meta_ = gfx::Text(meta, type::body);
        name_ = gfx::Text(toWide(p.name), type::displayM);
    }
    float preferredHeight(float) override { return kAvatar + 8; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        drawArtwork(c, profile_.images, {r.x, r.y, kAvatar, kAvatar}, kAvatar * 0.5f, Placeholder::Artist, true);
        const float tx = r.x + kAvatar + 28, tw = std::max(0.f, r.right() - tx);
        const float cy = r.y + kAvatar * 0.5f;
        c.text(toUpperTr(tr(L"Profil")), type::monoLabel, {tx, cy - 50, tw, 14}, col.fgSecondary);
        c.text(name_, {tx, cy - 32, tw, 48}, col.fgPrimary);
        if (!meta_.empty()) c.text(meta_, {tx, cy + 22, tw, 22}, col.fgSecondary);
    }

private:
    static constexpr float kAvatar = 136;
    Profile profile_;
    gfx::Text name_, meta_;
};

class ProfileView : public ScrollPage {
public:
    explicit ProfileView(std::string id) : id_(std::move(id)) {
        showSkeleton(4);
        load();
    }

private:
    struct Loaded {
        Profile profile;
        spotify::ProfilePlaylists first;
    };

    void load() {
        spotify::Api* api = source::activeApi();
        if (!api) {
            auto* c = resetContent();
            c->setPadding(gfx::metrics::pageX, 20, 48);
            c->add<MessagePanel>("user", tr(L"Spotify'a bağlı değilsin"), tr(L"Profilleri görmek için Spotify hesabınla bağlan."));
            contentReady();
            return;
        }
        api_ = api;
        async(Priority::High, life_.ref(),
              [api, id = id_] {
                  Loaded l;
                  l.profile = api->profile(id);
                  l.first = api->profilePlaylists(id, 0, kPage);
                  return l;
              },
              [this](Result<Loaded> r) {
                  if (!r) {
                      showError(r.errorMessage().find("404") != std::string::npos ? tr(L"Bu profil bulunamadı.")
                                                                                   : toWide(r.errorMessage()),
                                [this] {
                                    showSkeleton(4);
                                    load();
                                });
                      return;
                  }
                  build(std::move(*r));
              });
    }

    void build(Loaded l) {
        if (l.profile.publicPlaylists < 0) l.profile.publicPlaylists = l.first.total;
        playlists_ = std::move(l.first.playlists);
        next_ = l.first.nextOffset;
        auto* c = resetContent(24.f);
        c->setPadding(gfx::metrics::pageX, 28, 48);
        c->add<ProfileHeader>(l.profile);
        if (playlists_.empty()) {
            c->add<MessagePanel>("playlist", tr(L"Herkese açık çalma listesi yok"),
                                 tr(L"Bu kullanıcı profilinde henüz bir çalma listesi paylaşmamış."));
            contentReady();
            return;
        }
        c->add<SectionHeader>(tr(L"Herkese açık çalma listeleri"));
        grid_ = addCardRow(c, 160, 0);
        for (const auto& p : playlists_) addPlaylistCard(grid_, p);
        more_ = addLoadMore(c, [this] { loadMore(); });
        setLoadMore(more_, next_ >= 0, false);
        contentReady();
    }

    void loadMore() {
        if (loading_ || next_ < 0 || !api_) return;
        loading_ = true;
        setLoadMore(more_, true, true);
        async(Priority::High, life_.ref(), [api = api_, id = id_, offset = next_] { return api->profilePlaylists(id, offset, kPage); },
              [this](Result<spotify::ProfilePlaylists> r) {
                  loading_ = false;
                  if (!r) {
                      toast(tr(L"Daha fazla sonuç yüklenemedi"), true);
                      setLoadMore(more_, true, false);
                      return;
                  }
                  const size_t before = playlists_.size();
                  for (auto& p : r->playlists)
                      if (std::none_of(playlists_.begin(), playlists_.end(), [&](const Playlist& o) { return o.id == p.id; }))
                          playlists_.push_back(std::move(p));
                  next_ = playlists_.size() > before ? r->nextOffset : -1;
                  for (size_t i = before; i < playlists_.size(); ++i) addPlaylistCard(grid_, playlists_[i]);
                  setLoadMore(more_, next_ >= 0, false);
                  scroll_->contentChanged();
                  invalidate();
              });
    }

    static constexpr int kPage = 50;
    std::string id_;
    spotify::Api* api_ = nullptr;
    std::vector<Playlist> playlists_;
    int next_ = -1;
    bool loading_ = false;
    ui::Grid* grid_ = nullptr;
    ui::Button* more_ = nullptr;
};

} // namespace

std::unique_ptr<Page> makeProfilePage(const std::string& id) { return std::make_unique<ProfileView>(id); }

} // namespace st::app
