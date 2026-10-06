// Arkadaş etkinliği: the panel with what the people the user follows on Spotify are listening to (see app/Social.h).
#include "app/Social.h"

#include "app/Commands.h"
#include "app/Source.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

namespace {

constexpr float kRowH = 76, kPadX = 20, kAvatar = 36;
constexpr double kRefreshMs = 60'000;
constexpr int64_t kNowMs = 6 * 60'000;   // started less than this ago: still playing ("now")

// A newer click supersedes a play still loading its context.
Lifetime g_playLife;

std::string readFile(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool isPlaylist(const std::string& uri) { return uri.rfind("spotify:playlist:", 0) == 0; }
bool isAlbum(const std::string& uri) { return uri.rfind("spotify:album:", 0) == 0; }
bool isArtist(const std::string& uri) { return uri.rfind("spotify:artist:", 0) == 0; }

// The context shown on a row: what the track plays from, else its album.
std::string contextUriOf(const spotify::FriendActivity& a) { return a.contextUri.empty() ? a.track.album.id : a.contextUri; }
std::string contextNameOf(const spotify::FriendActivity& a) {
    return a.contextUri.empty() ? a.track.album.name : a.contextName;
}
const char* contextIcon(const std::string& uri) {
    return isPlaylist(uri) ? "playlist" : isAlbum(uri) ? "album" : isArtist(uri) ? "artist" : "music-note";
}
bool openable(const std::string& uri) { return isPlaylist(uri) || isAlbum(uri) || isArtist(uri); }

void openUri(const std::string& uri) {
    if (!ctx().router) return;
    if (isPlaylist(uri)) ctx().router->navigate({RouteKind::Playlist, uri});
    else if (isAlbum(uri)) ctx().router->navigate({RouteKind::Album, uri});
    else if (isArtist(uri)) ctx().router->navigate({RouteKind::Artist, uri});
}

// "5 dk" / "3 sa" / "2 g": how long ago the friend started the track.
std::wstring ago(int64_t timestampMs) {
    const int64_t nowMs = nowUnix() * 1000;
    const int64_t min = std::max<int64_t>(0, nowMs - timestampMs) / 60'000;
    if (min < 60) return i18n::format(tr(L"{} dk"), std::max<int64_t>(1, min));
    if (min < 24 * 60) return i18n::format(tr(L"{} sa"), min / 60);
    return i18n::format(tr(L"{} g"), min / (24 * 60));
}

// Plays the friend's track where they play it: in the playlist / album context (paging a playlist until the track
// shows up), else in the track's album, else alone.
void playFriendTrack(const spotify::FriendActivity& a) {
    auto* player = ctx().player;
    if (!player) return;
    if (a.track.id.rfind("spotify:track:", 0) != 0) {
        toast(tr(L"Bu öğe ShadeTube'da çalınamıyor"), true);
        return;
    }
    spotify::Api* api = source::activeApi();
    const player::PlayContext alone{"friend", toWide(a.userName)};
    if (!api) {   // dev demo: no session to load the context with
        player->playContext({a.track}, 0, alone);
        return;
    }
    struct Out {
        std::vector<catalog::Track> tracks;
        int index = -1;
        player::PlayContext context;
    };
    g_playLife.renew();
    async(
        Priority::High, g_playLife.ref(),
        [api, a, alone] {
            Out o;
            auto take = [&](std::vector<catalog::Track> tracks, player::PlayContext context) {
                for (size_t i = 0; i < tracks.size(); ++i)
                    if (tracks[i].id == a.track.id) {
                        o.tracks = std::move(tracks);
                        o.index = static_cast<int>(i);
                        o.context = std::move(context);
                        return true;
                    }
                return false;
            };
            const std::string& cu = a.contextUri;
            try {
                if (isPlaylist(cu)) {
                    std::vector<catalog::Track> all;
                    int offset = 0;
                    for (int page = 0; page < 10; ++page) {   // 1000 rows at most
                        auto pg = api->playlistTracks(cu, offset, 100);
                        const int next = pg.nextOffset >= 0 ? pg.nextOffset : offset + static_cast<int>(pg.items.size());
                        for (auto& t : pg.items) all.push_back(std::move(t));
                        const bool found = std::any_of(all.begin(), all.end(), [&](const catalog::Track& t) { return t.id == a.track.id; });
                        if (found || !pg.hasMore || next <= offset) break;
                        offset = next;
                    }
                    take(std::move(all), {"playlist:" + cu, toWide(a.contextName)});
                } else if (isAlbum(cu)) {
                    auto al = api->album(cu);
                    take(std::move(al.tracks), {"album:" + al.id, toWide(al.name)});
                }
            } catch (const std::exception& e) {
                ST_LOG_WARN("friends", "context {} not loaded: {}", cu, e.what());
            }
            if (o.index < 0 && isAlbum(a.track.album.id) && a.track.album.id != cu) {
                try {
                    auto al = api->album(a.track.album.id);
                    take(std::move(al.tracks), {"album:" + al.id, toWide(al.name)});
                } catch (const std::exception& e) {
                    ST_LOG_WARN("friends", "album {} not loaded: {}", a.track.album.id, e.what());
                }
            }
            if (o.index < 0) {
                o.tracks = {a.track};
                o.index = 0;
                o.context = alone;
            }
            return o;
        },
        [](Result<Out> r) {
            if (!r || !ctx().player) return;
            ctx().player->playContext(std::move(r->tracks), r->index, std::move(r->context));
        });
}

} // namespace

std::filesystem::path socialDemoDir() {
    static const std::filesystem::path dir = [] {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetEnvironmentVariableW(L"SHADETUBE_SPOTIFY_DEMO", buf, MAX_PATH);
        return n > 0 && n < MAX_PATH ? std::filesystem::path(buf) : std::filesystem::path();
    }();
    return dir;
}

bool socialAvailable() { return source::loggedIn() || !socialDemoDir().empty(); }

// ===================================================================================================
// FriendPanel

FriendPanel::FriendPanel() : title_(tr(L"Arkadaş etkinliği"), type::title) {
    clipsChildren = true;
    refresh_ = add<Button>(ButtonKind::Icon, L"", "refresh");
    refresh_->setIconSize(16);
    refresh_->setTooltip(tr(L"Yenile"));
    refresh_->onClick = [this] {
        if (!inFlight_) fetch();
    };
    close_ = add<Button>(ButtonKind::Icon, L"", "close");
    close_->setIconSize(16);
    close_->setTooltip(tr(L"Kapat"));
    close_->onClick = [] { commands::run("friend-activity"); };
    // The minute refresh rides on App's 2 s housekeeping tick (no timer of its own; nothing runs while hidden).
    ctx().housekeepingHooks.push_back([this, ref = life_.ref()] {
        if (!ref.expired()) tick();
    });
}

void FriendPanel::refreshIfStale() {
    if (!inFlight_ && ui::frame::realNow() - lastFetch_ >= kRefreshMs && ui::frame::realNow() >= retryAt_) fetch();
}

void FriendPanel::tick() {
    if (!visible() || !socialAvailable() || !ctx().window || !ctx().window->isShown()) return;
    refreshIfStale();
}

void FriendPanel::fetch() {
    lastFetch_ = ui::frame::realNow();
    if (const auto demo = socialDemoDir(); !demo.empty() && !source::loggedIn()) {
        const auto j = nlohmann::json::parse(readFile(demo / L"buddylist.json"), nullptr, false);
        loaded_ = true;
        setFriends(spotify::Api::parseBuddylist(j));
        return;
    }
    spotify::Api* api = source::activeApi();
    if (!api) return;
    inFlight_ = true;
    invalidate();
    async(Priority::Normal, life_.ref(), [api] { return api->friendActivity(); },
          [this](Result<std::vector<spotify::FriendActivity>> r) {
              inFlight_ = false;
              loaded_ = true;
              if (!r) {
                  int status = 0, retryAfter = 0;
                  try {
                      std::rethrow_exception(r.error());
                  } catch (const spotify::ApiError& e) {
                      status = e.status;
                      retryAfter = e.retryAfterSec;
                  } catch (...) {
                  }
                  // Rate limited: wait what Spotify asks (at least 5 minutes); other failures retry with the next tick.
                  if (status == 429) retryAt_ = ui::frame::realNow() + std::max(retryAfter, 300) * 1000.0;
                  ST_LOG_WARN("friends", "friend activity unavailable (HTTP {}): {}", status, r.errorMessage());
                  error_ = tr(L"Arkadaş etkinliği alınamadı.");
                  invalidate();
                  return;
              }
              error_.clear();
              ST_LOG_INFO("friends", "{} friend(s) with activity", r->size());
              setFriends(std::move(*r));
          });
}

void FriendPanel::setFriends(std::vector<spotify::FriendActivity> list) {
    rows_.clear();
    for (auto& a : list) {
        Row row{std::move(a), {}, {}, {}, {}};
        row.name = gfx::Text(toWide(row.a.userName), type::body.withSize(13));
        row.track = gfx::Text(toWide(row.a.track.name), type::caption);
        row.artist = gfx::Text(toWide(row.a.track.artistLine()), type::caption);
        row.context = gfx::Text(toWide(contextNameOf(row.a)), type::tiny);
        rows_.push_back(std::move(row));
    }
    rects_.clear();
    hover_ = pressed_ = {};
    scroll_.snap(std::clamp(scroll_.value(), 0.f, std::max(0.f, contentHeight() - (rect().h - listTop()))));
    invalidate();
}

float FriendPanel::contentHeight() const { return static_cast<float>(rows_.size()) * kRowH + 16; }

void FriendPanel::layout() {
    const float w = rect().w;
    close_->setRect({w - kPadX - 32, 20 + 2, 32, 32});
    refresh_->setRect({w - kPadX - 32 - 4 - 32, 20 + 2, 32, 32});
}

bool FriendPanel::onWheel(float dy, float, const ui::MouseEvent&) {
    const float maxS = std::max(0.f, contentHeight() - (rect().h - listTop()));
    scroll_.to(std::clamp(scroll_.target() - dy * 104.f, 0.f, maxS), ui::motion::medium, ui::Ease::Decelerate);
    invalidate();
    return true;
}

FriendPanel::Hit FriendPanel::hitAt(gfx::Point local) const {
    if (local.y < listTop()) return {};
    for (size_t i = 0; i < rects_.size(); ++i) {
        const auto& r = rects_[i];
        if (!r.row.contains(local)) continue;
        const int row = static_cast<int>(i);
        if (r.track.contains(local)) return {row, Part::Track};
        if (r.artist.contains(local)) return {row, Part::Artist};
        if (r.context.contains(local)) return {row, Part::Context};
        return {row, Part::None};
    }
    return {};
}

LPCWSTR FriendPanel::cursor() const { return hover_.part != Part::None ? IDC_HAND : IDC_ARROW; }

void FriendPanel::onMouseMove(const ui::MouseEvent& e) {
    const Hit h = hitAt({e.pos.x - rect().x, e.pos.y - rect().y});
    if (h != hover_) {
        hover_ = h;
        invalidate();
    }
}

void FriendPanel::onMouseLeave() {
    hover_ = {};
    invalidate();
}

bool FriendPanel::onMouseDown(const ui::MouseEvent& e) {
    const Hit h = hitAt({e.pos.x - rect().x, e.pos.y - rect().y});
    if (h.row < 0) return false;
    if (e.button == ui::MouseButton::Right) {
        const auto& t = rows_[h.row].a.track;
        if (t.id.rfind("spotify:track:", 0) == 0) showTrackMenu({t}, e.windowPos);
        return false;
    }
    if (e.button != ui::MouseButton::Left || h.part == Part::None) return false;
    pressed_ = h;   // acted on at release: opening a page here would replace widgets the window still holds
    return true;
}

void FriendPanel::onMouseUp(const ui::MouseEvent& e) {
    const Hit h = hitAt({e.pos.x - rect().x, e.pos.y - rect().y});
    const Hit pressed = std::exchange(pressed_, Hit{});
    if (h.row < 0 || h != pressed) return;
    activate(h, e.windowPos);
}

void FriendPanel::activate(const Hit& h, gfx::Point) {
    if (h.row < 0 || h.row >= static_cast<int>(rows_.size())) return;
    const auto a = rows_[h.row].a;   // copied: opening a page may refresh the list
    switch (h.part) {
    case Part::Track: playFriendTrack(a); break;
    case Part::Artist:
        if (!a.track.artists.empty()) openUri(a.track.artists[0].id);
        break;
    case Part::Context: openUri(contextUriOf(a)); break;
    case Part::None: break;
    }
}

void FriendPanel::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    const auto& acc = accent();
    c.fillRect(r, col.bgBase.withAlpha(0.35f));
    c.vline(r.x, r.y, r.bottom(), col.hairSubtle);
    c.pushClip(r);
    c.pushTransform(D2D1::Matrix3x2F::Translation(r.x, r.y));
    const float titleRight = refresh_->rect().x - 8;
    c.text(title_, {kPadX, 20, titleRight - kPadX, 36}, col.fgPrimary, gfx::VAlign::Center);

    auto message = [&](const std::wstring& head, const std::wstring& body) {
        float y = listTop() + 8;
        if (!head.empty()) {
            c.text(head, type::body, {kPadX, y, r.w - kPadX * 2, 20}, col.fgPrimary);
            y += 26;
        }
        gfx::Text t(body, type::secondary, {gfx::TextAlign::Leading, true, true, 6});
        c.text(t, {kPadX, y, r.w - kPadX * 2, 120}, col.fgTertiary);
    };
    rects_.clear();
    if (rows_.empty()) {
        if (!loaded_ || inFlight_) message({}, tr(L"Yükleniyor…"));
        else if (!error_.empty()) message(error_, tr(L"Bir dakika içinde yeniden denenecek. Hemen denemek için yenile."));
        else
            message(tr(L"Henüz etkinlik yok"),
                    tr(L"Spotify'da takip ettiğin kişilerin dinledikleri burada görünür. Yalnızca dinleme etkinliğini "
                       L"paylaşanlar listelenir."));
    } else {
        c.pushClip({0, listTop() - 4, r.w, r.h - listTop() + 4});
        const float textX = kPadX + kAvatar + 12, right = r.w - kPadX;
        const int64_t nowMs = nowUnix() * 1000;
        for (size_t i = 0; i < rows_.size(); ++i) {
            auto& row = rows_[i];
            const Rect rr{0, listTop() + static_cast<float>(i) * kRowH - scroll_.value(), r.w, kRowH};
            Rects hit{rr, {}, {}, {}};
            if (rr.bottom() < listTop() - 4 || rr.y > r.h) {
                rects_.push_back(hit);
                continue;
            }
            const bool rowHover = hover_.row == static_cast<int>(i);
            if (rowHover) c.fillRect(rr, col.overlayHover);
            const Rect avatar{kPadX, rr.y + 12, kAvatar, kAvatar};
            std::vector<catalog::Image> img;
            if (!row.a.imageUrl.empty()) img.push_back({row.a.imageUrl, 300, 300});
            drawArtwork(c, img, avatar, 0, Placeholder::Artist, true, Priority::Normal);

            // Line 1: name + "now" (accent equalizer) or how long ago.
            const bool now = row.a.timestampMs > 0 && nowMs - row.a.timestampMs < kNowMs;
            float nameRight = right;
            if (now) {
                c.icon("equalizer", {right - 14, rr.y + 12, 14, 14}, acc.base);
                nameRight -= 22;
            } else if (row.a.timestampMs > 0) {
                const std::wstring when = ago(row.a.timestampMs);
                c.text(when, type::monoMeta, {right - 60, rr.y + 10, 60, 18}, col.fgTertiary, gfx::TextAlign::Trailing,
                       gfx::VAlign::Center);
                nameRight -= 64;
            }
            c.text(row.name, {textX, rr.y + 10, nameRight - textX, 18}, col.fgPrimary, gfx::VAlign::Center);

            // Line 2: track · artist (each a link).
            const float avail = right - textX;
            const float trackW = std::min(std::ceil(row.track.measure().w), avail * (row.artist.empty() ? 1.f : 0.62f));
            hit.track = {textX, rr.y + 30, trackW, 16};
            const bool trackHot = rowHover && hover_.part == Part::Track;
            c.text(row.track, hit.track, trackHot ? col.fgPrimary : col.fgSecondary, gfx::VAlign::Center);
            if (!row.artist.empty()) {
                const float dotX = textX + trackW + 4;
                c.text(L"·", type::caption, {dotX, rr.y + 30, 8, 16}, col.fgTertiary, gfx::TextAlign::Center, gfx::VAlign::Center);
                const float ax = dotX + 12;
                const float aw = std::max(0.f, std::min(std::ceil(row.artist.measure().w), right - ax));
                hit.artist = {ax, rr.y + 30, aw, 16};
                const bool artistHot = rowHover && hover_.part == Part::Artist;
                c.text(row.artist, hit.artist, artistHot ? col.fgPrimary : col.fgSecondary, gfx::VAlign::Center);
                if (row.a.track.artists.empty() || !isArtist(row.a.track.artists[0].id)) hit.artist = {};
            }

            // Line 3: the context it plays from.
            const std::string cu = contextUriOf(row.a);
            if (!row.context.empty()) {
                const bool ctxHot = rowHover && hover_.part == Part::Context;
                const Color cc = ctxHot ? col.fgPrimary : col.fgTertiary;
                c.icon(contextIcon(cu), {textX, rr.y + 52, 12, 12}, cc);
                const float cx = textX + 18;
                const float cw = std::max(0.f, std::min(std::ceil(row.context.measure().w), right - cx));
                c.text(row.context, {cx, rr.y + 50, cw, 16}, cc, gfx::VAlign::Center);
                if (openable(cu)) hit.context = {textX, rr.y + 50, cw + 18, 16};
            }
            rects_.push_back(hit);
        }
        c.popClip();
    }
    c.popTransform();
    c.popClip();
    paintChildren(c);
}

} // namespace st::app
