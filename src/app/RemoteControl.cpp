// Phone remote control ("Telefondan kumanda"): the app side of the LAN server (app/RemoteServer, RemoteApi, RemoteAuth,
// RemoteState) and its Ayarlar section.
//
// Off by default (Settings::remoteEnabled). When on, the server listens on its own thread; the phone opens the page
// from the QR code (assets/remote/*, nothing to install), pairs once with the secret in the URL fragment and from then
// on sends its own device token. Everything the phone asks for is posted to the UI thread (st::Dispatcher) and done
// through the same code paths as the player bar, the queue panel and the track menus (commands::run, Player,
// smartshuffle, Library). The phone is kept current with Server-Sent Events: "state" and "queue" on every player
// change (a playerChangedHooks entry, coalesced per UI turn), "pos" once a second only while a phone has the event
// stream open (the server's tick), with the synced lyrics line when the lyrics are at hand.
#include "app/AppContext.h"
#include "app/Commands.h"
#include "app/InternetRadio.h"
#include "app/LyricsService.h"
#include "app/RemoteApi.h"
#include "app/RemoteAuth.h"
#include "app/RemoteServer.h"
#include "app/RemoteState.h"
#include "app/SettingsWidgets.h"
#include "app/Shortcuts.h"
#include "app/SmartShuffle.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/Async.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Resources.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>
#include <qrcodegen.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <unordered_map>

namespace st::app {

namespace {

using json = nlohmann::json;
using catalog::Track;
namespace type = gfx::type;

constexpr const char* kTag = "remote";
constexpr int kDefaultPort = 48484;
constexpr int kQueueWindow = 100;       // queue items the phone sees after the playing one
constexpr size_t kMaxLists = 16;        // answers (search, album, playlist, artist) kept for the play commands
constexpr int kPlaylistPages = 3;       // a Spotify playlist / Liked Songs: up to 300 songs for the phone
constexpr uintmax_t kMaxCoverBytes = 8u << 20;

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

std::string fnvHex(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// "file:///C:/Music/a%20b.jpg" -> L"C:\\Music\\a b.jpg"; "" for anything else.
std::wstring filePathOf(const std::string& url) {
    if (url.rfind("file:///", 0) != 0) return {};
    std::string decoded;
    if (!remote::percentDecode(std::string_view(url).substr(8), false, decoded)) return {};
    std::wstring p = toWide(decoded);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p;
}

const char* imageType(const std::wstring& path) {
    std::wstring ext = std::filesystem::path(path).extension().wstring();
    for (auto& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    if (ext == L".jpg" || ext == L".jpeg") return "image/jpeg";
    if (ext == L".png") return "image/png";
    if (ext == L".webp") return "image/webp";
    if (ext == L".gif") return "image/gif";
    if (ext == L".bmp") return "image/bmp";
    return nullptr;
}

const char* statusName(player::Status s) {
    switch (s) {
    case player::Status::Idle: return "idle";
    case player::Status::Resolving: return "resolving";
    case player::Status::Buffering: return "buffering";
    case player::Status::Playing: return "playing";
    case player::Status::Paused: return "paused";
    case player::Status::Error: return "error";
    }
    return "idle";
}

std::string hex(const gfx::Color& c) { return remote::hexColor(c.r, c.g, c.b); }

remote::Response ok(const std::wstring& toastText = {}) {
    json j = {{"ok", true}};
    if (!toastText.empty()) j["toast"] = toUtf8(toastText);
    return remote::Response::json(200, dump(j));
}

remote::Response refused(int status, const std::wstring& toastText) {
    return remote::Response::json(status, dump({{"error", remote::reasonPhrase(status)}, {"toast", toUtf8(toastText)}}));
}

std::wstring countText(int n) { return n > 0 ? i18n::plural(L"{} şarkı", n) : std::wstring{}; }

// The page's texts, in the UI language (the page never shows a hard-coded word).
std::string pageStrings() {
    const std::pair<const char*, std::wstring> texts[] = {
        {"title", tr(L"Telefondan kumanda")},
        {"pairHint", tr(L"Bilgisayarındaki ShadeTube'da Ayarlar › Telefondan kumanda bölümünü aç ve QR kodu bu telefonun "
                        L"kamerasıyla tara.")},
        {"codeLabel", tr(L"Bağlantı kodu")},
        {"codeHint", tr(L"QR kodu tarayamıyorsan bilgisayarda görünen bağlantı kodunu buraya yaz.")},
        {"connect", tr(L"Bağlan")},
        {"pairing", tr(L"Eşleştiriliyor…")},
        {"pairFailed", tr(L"Bağlantı kodu geçersiz ya da yenilenmiş. QR kodu yeniden tara.")},
        {"revoked", tr(L"Bu telefonun bağlantısı kesildi. Yeniden bağlanmak için QR kodu tara.")},
        {"throttled", tr(L"Çok fazla hatalı deneme. Biraz bekleyip yeniden dene.")},
        {"offline", tr(L"ShadeTube'a ulaşılamıyor. Yeniden bağlanılıyor…")},
        {"disconnectPhone", tr(L"Bu telefonun bağlantısını kes")},
        {"tabNow", tr(L"Şimdi çalıyor")},
        {"tabQueue", tr(L"Sıradaki")},
        {"tabSearch", tr(L"Ara")},
        {"tabLibrary", tr(L"Kitaplık")},
        {"nowLabel", tr(L"ŞİMDİ ÇALIYOR")},
        {"nothing", tr(L"Şu an bir şey çalmıyor")},
        {"nothingHint", tr(L"Ara sekmesinden bir şarkı bul ya da kitaplığından bir liste aç.")},
        {"play", tr(L"Oynat")},
        {"pause", tr(L"Duraklat")},
        {"next", tr(L"Sonraki")},
        {"prev", tr(L"Önceki")},
        {"shuffle", tr(L"Karıştır")},
        {"smartShuffle", tr(L"Akıllı karıştırma")},
        {"repeat", tr(L"Tekrarla")},
        {"like", tr(L"Beğen")},
        {"volume", tr(L"Ses düzeyi")},
        {"live", tr(L"Canlı")},
        {"queueNow", tr(L"ŞİMDİ")},
        {"queueNext", tr(L"SONRAKİ")},
        {"queueEmpty", tr(L"Sıra boş. Bir şarkı çal, burada görünsün.")},
        {"playNow", tr(L"Şimdi çal")},
        {"remove", tr(L"Sıradan kaldır")},
        {"searchPlaceholder", tr(L"Ne dinlemek istersin?  Sanatçı, albüm, şarkı")},
        {"songs", tr(L"Şarkılar")},
        {"albums", tr(L"Albümler")},
        {"playlists", tr(L"Çalma listeleri")},
        {"artists", tr(L"Sanatçılar")},
        {"noResults", tr(L"Sonuç bulunamadı")},
        {"searchFailed", tr(L"Arama yapılamadı. Bağlantını kontrol et.")},
        {"error", tr(L"Bir şeyler ters gitti")},
        {"retry", tr(L"Tekrar dene")},
        {"loading", tr(L"Yükleniyor…")},
        {"playSong", tr(L"Çal")},
        {"playNext", tr(L"Sonra çal")},
        {"addToQueue", tr(L"Sıraya ekle")},
        {"goAlbum", tr(L"Albüme git")},
        {"goArtist", tr(L"Sanatçıya git")},
        {"cancel", tr(L"Vazgeç")},
        {"back", tr(L"Geri")},
        {"album", tr(L"Albüm")},
        {"single", tr(L"Tekli")},
        {"playlist", tr(L"Çalma listesi")},
        {"artist", tr(L"Sanatçı")},
        {"popular", tr(L"Popüler")},
        {"likedSongs", tr(L"Beğenilen Şarkılar")},
        {"libraryEmpty", tr(L"Kitaplığında henüz bir şey yok")},
    };
    json s = json::object();
    for (const auto& [key, text] : texts) s[key] = toUtf8(text);
    return dump({{"lang", i18n::code()}, {"s", std::move(s)}});
}

struct RemoteList {
    uint32_t handle = 0;
    player::PlayContext context;
    std::vector<Track> tracks;
};

// The playlists of the user's library as the phone lists them: Liked Songs first (id "liked"), then the playlists
// (Spotify's while logged in, else the local ones).
std::vector<std::pair<std::string, catalog::Playlist>> libraryPlaylists() {
    std::vector<std::pair<std::string, catalog::Playlist>> out;
    if (source::loggedIn()) {
        for (const auto& p : ctx().session->library().playlists) {
            if (p.id == spotify::Api::kLikedSongsUri) {
                catalog::Playlist liked = p;
                liked.name = toUtf8(tr(L"Beğenilen Şarkılar"));
                liked.images.clear();
                out.insert(out.begin(), {"liked", liked});
            } else {
                out.emplace_back(p.id, p);
            }
        }
    } else {
        catalog::Playlist liked;
        liked.name = toUtf8(tr(L"Beğenilen Şarkılar"));
        liked.totalTracks = static_cast<int>(ctx().library.liked().size());
        out.emplace_back("liked", liked);
        for (const auto& p : ctx().library.playlists()) out.emplace_back(p.id, p);
    }
    return out;
}

class RemoteControl {
public:
    static RemoteControl& get() {
        static RemoteControl r;
        return r;
    }

    std::function<void()> onChanged;   // the Ayarlar section (one at a time): status, devices, address

    void init() {
        auto& c = ctx();
        c.playerChangedHooks.push_back([this] { schedulePush(); });
        c.housekeepingHooks.push_back([this] {
            if (server_ && store_) store_->saveIfDirty(nowUnix(), 300);   // last-seen times, now and then
        });
        c.persistHooks.push_back([this] { stop(); });   // exit: close the listener and every phone's connection
        // A like / unlike anywhere (the heart on the phone follows it).
        c.library.subscribe(hooks_.ref(), [this] { schedulePush(); });
        if (c.session) c.session->subscribe(hooks_.ref(), [this] { schedulePush(); });
        apply();
    }

    // Starts or stops the server to match Settings::remoteEnabled.
    void apply() {
        const bool want = Settings::get().remoteEnabled;
        if (want && !server_) start();
        else if (!want) {
            stop();
            failure_.clear();
            portsInUse_ = false;
        }
    }
    void restart() {
        stop();
        if (Settings::get().remoteEnabled) start();
    }

    bool running() const { return server_ && server_->running(); }
    bool portsInUse() const { return portsInUse_; }
    uint16_t port() const { return server_ ? server_->port() : 0; }

    remote::DeviceStore& store() {
        if (!store_) {
            store_ = std::make_unique<remote::DeviceStore>(paths::appData() / L"remote.dat");
            store_->load();
        }
        return *store_;
    }

    const std::vector<remote::LanAddress>& addresses() const { return addresses_; }
    void refreshAddresses() {
        addresses_ = remote::lanAddresses();
        std::string list;
        for (const auto& a : addresses_) list += a.ip + (a.isVirtual ? "(v) " : " ");
        if (list != loggedAddresses_) ST_LOG_INFO(kTag, "LAN addresses: {}", list.empty() ? std::string("none") : list);
        loggedAddresses_ = std::move(list);
    }
    // The address the QR code points at: the chosen adapter while it is there, else the best one; the bind address
    // itself when the server listens on one interface only (SHADETUBE_REMOTE_BIND). "" = no network.
    std::string host() const {
        if (bindAddress_ != "0.0.0.0") return bindAddress_;
        const std::string& want = Settings::get().remoteAddress;
        for (const auto& a : addresses_)
            if (!want.empty() && a.ip == want) return a.ip;
        return addresses_.empty() ? std::string{} : addresses_.front().ip;
    }
    std::string baseUrl() const {
        const std::string h = host();
        return h.empty() || !running() ? std::string{} : "http://" + h + ":" + std::to_string(port());
    }
    std::string pairingUrl() {
        const std::string base = baseUrl();
        return base.empty() ? std::string{} : base + "/#p=" + store().pairingSecret();
    }

    std::vector<std::string> connectedDevices() const { return server_ ? server_->streamDevices() : std::vector<std::string>{}; }

    void rotateSecret() {
        store().rotateSecret();
        notify();
    }
    void revoke(const remote::Device& d) {
        store().revoke(d.id);
        if (server_) server_->dropDevice(d.id);
        notify();
    }

private:
    void start() {
        failure_.clear();
        portsInUse_ = false;
        store();
        if (!router_) {
            remote::Router::Config cfg;
            cfg.asset = [](std::wstring_view name) { return res::load(name); };
            cfg.strings = [this] { return strings_; };   // fixed while the server runs
            // {"state": ..., "queue": ...}: what a new event stream starts with (for browsers that can only poll).
            cfg.state = [this] {
                const std::string s = server_ ? server_->latest("state") : std::string{};
                const std::string q = server_ ? server_->latest("queue") : std::string{};
                return "{\"state\":" + (s.empty() ? std::string("null") : s) + ",\"queue\":" + (q.empty() ? std::string("null") : q) + "}";
            };
            router_ = std::make_unique<remote::Router>(*store_, std::move(cfg));
        }
        strings_ = pageStrings();
        wchar_t bind[64] = {};
        bindAddress_ = GetEnvironmentVariableW(L"SHADETUBE_REMOTE_BIND", bind, 64) > 0 ? toUtf8(bind) : "0.0.0.0";

        // Server-thread callbacks: everything goes to the UI thread, dropped once the server stopped (life_).
        const Lifetime::Ref ref = life_.ref();
        router_->onCall = [this, ref](remote::ApiCall call, remote::Reply reply) {
            Dispatcher::post([this, ref, call = std::move(call), reply = std::move(reply)]() mutable {
                if (!ref.expired()) handle(std::move(call), std::move(reply));
            });
        };
        router_->onPaired = [this, ref](const remote::Device& d) {
            Dispatcher::post([this, ref, d] {
                if (ref.expired()) return;
                const std::wstring name = d.name.empty() ? std::wstring(tr(L"Bilinmeyen tarayıcı")) : toWide(d.name);
                toast(i18n::format(tr(L"Yeni telefon bağlandı: {}"), {name}), false, true);
                notify();
            });
        };
        router_->onUnpaired = [this, ref](const remote::Device&) {
            Dispatcher::post([this, ref] {
                if (!ref.expired()) notify();
            });
        };
        server_ = std::make_unique<remote::Server>(*router_);
        server_->onTick = [this, ref] {
            if (tickPending_.exchange(true)) return;   // the UI thread hasn't run the last one yet
            Dispatcher::post([this, ref] {
                tickPending_ = false;
                if (!ref.expired()) tick();
            });
        };
        server_->onStreamsChanged = [this, ref] {
            Dispatcher::post([this, ref] {
                if (!ref.expired()) notify();
            });
        };
        remote::Server::Options o;
        o.bindAddress = bindAddress_;
        o.port = static_cast<uint16_t>(std::clamp(Settings::get().remotePort, 1024, 65535));
        std::string error;
        bool inUse = false;
        if (!server_->start(o, &error, &inUse)) {
            failure_ = error.empty() ? std::string("start failed") : error;
            portsInUse_ = inUse;
            server_.reset();
            life_.renew();
            notify();
            return;
        }
        refreshAddresses();
        pushState(true);
        pushQueue();
        notify();
    }

    void stop() {
        life_.renew();
        if (server_) server_->stop();
        server_.reset();
        if (store_) store_->saveIfDirty(nowUnix(), 0);
        lastState_.clear();
        lastQueue_.clear();
        lists_.clear();
        covers_.clear();
        lyrics_.reset();
        lyricsFor_.clear();
        lyricsLife_.renew();
        notify();
    }

    void notify() {
        if (onChanged) {
            const auto fn = onChanged;   // may rebuild the page, which sets onChanged again
            fn();
        }
    }

    // ---- What the phone sees -----------------------------------------------------------------------------------------

    remote::CoverMapper coverMapper() {
        return [this](const std::string& url) -> std::string {
            if (url.rfind("file:///", 0) != 0) return {};
            if (covers_.size() > 4096) covers_.clear();
            const std::string h = fnvHex(url);
            covers_.emplace(h, url);
            return "/api/cover/" + h;
        };
    }

    remote::NowPlaying nowPlaying() const {
        remote::NowPlaying s;
        auto* p = ctx().player;
        if (!p) return s;
        const Track* t = p->current();
        s.track = t;
        s.status = statusName(p->status());
        s.playing = p->isPlaying() || p->status() == player::Status::Resolving;   // like SMTC: Pause while resolving
        s.live = p->isLive();
        s.podcast = t && catalog::isPodcastId(t->id);
        s.liveTitle = toUtf8(p->liveTitle());
        s.positionMs = p->positionMs();
        s.durationMs = p->durationMs() > 0 ? p->durationMs() : (t ? t->durationMs : 0);
        s.speed = p->speed();
        const auto mode = smartshuffle::mode();
        s.shuffle = mode == smartshuffle::Mode::Smart ? "smart" : mode == smartshuffle::Mode::Shuffle ? "on" : "off";
        const auto rep = p->repeat();
        s.repeat = rep == RepeatMode::One ? "one" : rep == RepeatMode::All ? "all" : "off";
        if (t) {
            // A station: the heart is the radio favorite (like the player bar's).
            const bool station = radio::isStationId(t->id);
            s.liked = station ? radio::store().isFavorite(radio::uuidOf(t->id)) : ctx().library.isLiked(t->id);
            s.canLike = station || ctx().library.canLike(t->id);
        }
        s.hasNext = p->order().size() > 1;
        s.volume = p->volume();
        const gfx::Color a = gfx::Theme::get().accentTarget();
        const gfx::Accent dark = gfx::Accent::derive(a, false);
        const gfx::Accent light = gfx::Accent::derive(gfx::Theme::adaptAccent(a, true), true);
        s.accentDark = {hex(dark.base), hex(dark.onAccent)};
        s.accentLight = {hex(light.base), hex(light.onAccent)};
        s.contextName = toUtf8(p->context().name);
        return s;
    }

    void schedulePush() {
        if (!server_ || pushPending_) return;
        pushPending_ = true;
        Dispatcher::post([this, ref = life_.ref()] {
            pushPending_ = false;
            if (ref.expired()) return;
            pushState(true);
            pushQueue();
        });
    }

    // `force`: a player event (also a seek, which changes nothing but the position). Otherwise only when something
    // other than the position changed (the volume knob, the accent fading in, a like made elsewhere).
    void pushState(bool force) {
        if (!server_) return;
        remote::NowPlaying np = nowPlaying();
        const auto covers = coverMapper();
        std::string full = remote::stateJson(np, covers);
        np.positionMs = 0;
        std::string sig = remote::stateJson(np, covers);
        if (!force && sig == lastState_) return;
        lastState_ = std::move(sig);
        server_->broadcast("state", std::move(full));
    }

    void pushQueue() {
        auto* p = ctx().player;
        if (!server_ || !p) return;
        remote::QueueView q;
        const auto& order = p->order();
        const auto& items = p->items();
        const int pos = p->currentOrderIndex();
        q.current = pos;
        const int first = std::max(0, pos);
        const int end = std::min(static_cast<int>(order.size()), first + 1 + kQueueWindow);
        for (int i = first; i < end; ++i) {
            const int idx = order[static_cast<size_t>(i)];
            if (idx >= 0 && idx < static_cast<int>(items.size())) q.items.emplace_back(i, &items[static_cast<size_t>(idx)]);
        }
        q.more = static_cast<int>(order.size()) - end;
        if (q.more > 0) q.moreText = toUtf8(i18n::plural(L"ve {} şarkı daha", q.more));
        std::string body = remote::queueJson(q, coverMapper());
        if (body == lastQueue_) return;
        lastQueue_ = body;
        server_->broadcast("queue", std::move(body));
    }

    // Once a second while a phone has the event stream open.
    void tick() {
        auto* p = ctx().player;
        if (!server_ || !p) return;
        pushState(false);
        const bool playing = p->isPlaying() || p->status() == player::Status::Resolving;
        server_->broadcast("pos", remote::positionJson(p->positionMs(), playing, lyricLine(p->current())));
    }

    // The synced lyrics line at the player position (the lyrics views' clock: offsets and SponsorBlock cuts applied).
    // The lyrics are fetched once per track, only while a phone is looking (from the disk cache when the PC showed
    // them already).
    std::string lyricLine(const Track* t) {
        auto* p = ctx().player;
        if (!t || !Settings::get().lyricsEnabled || p->isLive() || catalog::isPodcastId(t->id)) return {};
        if (lyricsFor_ != t->id) {
            lyricsFor_ = t->id;
            lyrics_.reset();
            lyricsLife_.renew();
            const lyrics::Query q = lyricsQueryFor(*t);
            async(Priority::Low, lyricsLife_.ref(), [q] { return lyrics::fetch(q); },
                  [this, id = t->id](Result<std::optional<lyrics::Lyrics>> r) {
                      if (r && *r && (*r)->synced && lyricsFor_ == id)
                          lyrics_ = std::make_shared<const lyrics::Lyrics>(std::move(**r));
                  });
            return {};
        }
        if (!lyrics_ || lyrics_->lines.empty()) return {};
        const int clock = lyricsClockMs(t->id);
        const auto& lines = lyrics_->lines;
        const auto it = std::upper_bound(lines.begin(), lines.end(), clock,
                                         [](int c, const lyrics::Line& l) { return c < l.timeMs; });
        return it == lines.begin() ? std::string{} : std::prev(it)->text;
    }

    // ---- API calls (UI thread) -----------------------------------------------------------------------------------------

    void handle(remote::ApiCall call, remote::Reply reply) {
        try {
            if (call.name == "cmd") return reply(command(call.body));
            if (call.name == "library") return reply(library());
            if (call.name == "search") return search(call.arg, std::move(reply));
            if (call.name == "album") return album(call.arg, std::move(reply));
            if (call.name == "playlist") return playlist(call.arg, std::move(reply));
            if (call.name == "artist") return artist(call.arg, std::move(reply));
            if (call.name == "cover") return cover(call.arg, std::move(reply));
            reply(remote::Response::error(404));
        } catch (const std::exception& e) {   // a JSON field of the wrong type
            ST_LOG_WARN(kTag, "{} failed: {}", call.name, e.what());
            reply(remote::Response::error(400));
        }
    }

    uint32_t addList(player::PlayContext context, std::vector<Track> tracks) {
        RemoteList l;
        l.handle = nextList_++;
        l.context = std::move(context);
        l.tracks = std::move(tracks);
        lists_.push_back(std::move(l));
        while (lists_.size() > kMaxLists) lists_.pop_front();
        return lists_.back().handle;
    }
    const RemoteList* findList(uint32_t handle) const {
        for (const auto& l : lists_)
            if (l.handle == handle) return &l;
        return nullptr;
    }

    json tracksJson(const std::vector<Track>& tracks) {
        const auto covers = coverMapper();
        json a = json::array();
        for (const auto& t : tracks) a.push_back(remote::trackJson(t, covers));
        return a;
    }

    remote::Response command(const json& b) {
        auto* p = ctx().player;
        if (!p) return remote::Response::error(503);
        const std::string cmd = b.value("cmd", "");
        // The player bar's buttons (and the keyboard shortcuts behind them).
        static const std::pair<const char*, const char*> kActions[] = {
            {"toggle", "play-pause"}, {"next", "next"}, {"prev", "previous"}, {"like", "like"}};
        for (const auto& [name, action] : kActions)
            if (cmd == name) {
                commands::run(action);
                return ok();
            }
        if (cmd == "shuffle") {
            smartshuffle::cycleMode();   // off -> shuffle -> smart shuffle, like the player bar's button
            return ok();
        }
        if (cmd == "repeat") {
            p->cycleRepeat();
            return ok();
        }
        if (cmd == "seek") {
            const int64_t ms = b.value("ms", int64_t{-1});
            if (ms < 0 || !p->current() || p->isLive()) return remote::Response::error(400);
            p->seek(ms);
            return ok();
        }
        if (cmd == "volume") {
            const double v = b.value("value", -1.0);
            if (!(v >= 0 && v <= 1)) return remote::Response::error(400);
            p->setVolume(static_cast<float>(v));
            if (p->onChanged) p->onChanged();   // the player bar's knob follows (like the volume shortcuts)
            return ok();
        }
        // The queue panel: a row's "Şimdi çal" / "Sıradan kaldır". The phone names the slot and the song in it, so a
        // queue that changed meanwhile is never touched in the wrong place.
        if (cmd == "jump" || cmd == "remove") {
            const int i = b.value("index", -1);
            const std::string id = b.value("id", "");
            const auto& order = p->order();
            if (i < 0 || i >= static_cast<int>(order.size()) || p->items()[static_cast<size_t>(order[static_cast<size_t>(i)])].id != id)
                return refused(409, tr(L"Sıra bu arada değişti"));
            if (cmd == "jump") p->jumpTo(i);
            else p->removeAt(i);
            return ok();
        }
        // A list the phone got from search / album / playlist / artist: play it (from a song), or a song / all of it
        // next or at the end of the queue (the track menu's "Sonra çal" / "Sıraya ekle").
        if (cmd == "playList" || cmd == "playNext" || cmd == "enqueue") {
            const RemoteList* l = findList(b.value("list", 0u));
            const int i = b.value("index", -1);
            if (!l || l->tracks.empty()) return refused(409, tr(L"Bir şeyler ters gitti"));
            if (i >= static_cast<int>(l->tracks.size())) return remote::Response::error(400);
            if (cmd == "playList") {
                if (b.value("shuffle", false)) p->setShuffle(true);
                p->playContext(l->tracks, i, l->context);   // -1: the player's own pick (shuffle) or the first
                return ok();
            }
            const std::vector<Track> picked = i < 0 ? l->tracks : std::vector<Track>{l->tracks[static_cast<size_t>(i)]};
            if (cmd == "playNext") {
                for (auto it = picked.rbegin(); it != picked.rend(); ++it) p->playNext(*it);
                return ok(tr(L"Sıradaki olarak eklendi"));
            }
            p->enqueue(picked);
            return ok(tr(L"Sıraya eklendi"));
        }
        return remote::Response::error(400);
    }

    remote::Response library() {
        const auto covers = coverMapper();
        json pls = json::array(), albums = json::array(), artists = json::array();
        for (const auto& [id, p] : libraryPlaylists())
            pls.push_back(remote::playlistJson(p, id, p.countKnown ? toUtf8(countText(p.totalTracks)) : std::string{}, covers));
        const bool spotify = source::loggedIn();
        for (const auto& a : spotify ? ctx().session->library().albums : ctx().library.albums())
            albums.push_back(remote::albumJson(a, covers));
        for (const auto& a : spotify ? ctx().session->library().artists : ctx().library.artists())
            artists.push_back(remote::artistJson(a, covers));
        return remote::Response::json(200, dump({{"playlists", pls}, {"albums", albums}, {"artists", artists}}));
    }

    void search(const std::string& q, remote::Reply reply) {
        // The user's own playlists that match (Spotify's search has no playlists of the library).
        json playlists = json::array();
        {
            const auto covers = coverMapper();
            const std::wstring fq = shortcuts::fold(toWide(q));
            std::vector<std::pair<int, json>> hits;
            for (const auto& [id, p] : libraryPlaylists()) {
                const int score = shortcuts::fuzzyScore(fq, shortcuts::fold(toWide(p.name)));
                if (score >= 0)
                    hits.emplace_back(score, remote::playlistJson(p, id, p.countKnown ? toUtf8(countText(p.totalTracks)) : "", covers));
            }
            std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            for (size_t i = 0; i < hits.size() && i < 20; ++i) playlists.push_back(std::move(hits[i].second));
        }
        spotify::Api* api = source::activeApi();
        async(Priority::High, life_.ref(), [api, q] { return source::search(api, q, 20); },
              [this, reply, playlists = std::move(playlists)](Result<catalog::SearchResults> r) mutable {
                  if (!r) {
                      ST_LOG_INFO(kTag, "search failed: {}", r.errorMessage());
                      reply(refused(502, tr(L"Arama yapılamadı. Bağlantını kontrol et.")));
                      return;
                  }
                  const auto covers = coverMapper();
                  json albums = json::array(), artists = json::array();
                  for (const auto& a : r->albums) albums.push_back(remote::albumJson(a, covers));
                  for (const auto& a : r->artists) artists.push_back(remote::artistJson(a, covers));
                  json j = {{"tracks", tracksJson(r->tracks)}, {"albums", albums}, {"artists", artists}, {"playlists", playlists}};
                  j["list"] = addList({"search", tr(L"Arama sonuçları")}, r->tracks);
                  reply(remote::Response::json(200, dump(j)));
              });
    }

    void album(const std::string& id, remote::Reply reply) {
        spotify::Api* api = source::activeApi();
        if (source::isSpotifyId(id) && !api) return reply(refused(409, tr(L"Spotify bağlantısı yok")));
        async(Priority::High, life_.ref(), [api, id] { return source::album(api, id); },
              [this, id, reply](Result<catalog::Album> r) {
                  if (!r) return reply(refused(502, tr(L"Bir şeyler ters gitti")));
                  json j = {{"album", remote::albumJson(*r, coverMapper(), remote::kCoverPx)},
                            {"count", toUtf8(countText(static_cast<int>(r->tracks.size())))},
                            {"tracks", tracksJson(r->tracks)}};
                  // The context the album page plays with ("album:<id>").
                  j["list"] = addList({"album:" + id, toWide(r->name)}, r->tracks);
                  reply(remote::Response::json(200, dump(j)));
              });
    }

    void sendPlaylist(const remote::Reply& reply, const std::string& id, const catalog::Playlist& meta,
                      std::vector<Track> tracks, int total) {
        json j = {{"playlist", remote::playlistJson(meta, id, toUtf8(countText(total)), coverMapper(), remote::kCoverPx)},
                  {"tracks", tracksJson(tracks)}};
        // The contexts the collection page plays with ("liked" / "playlist:<id>").
        j["list"] = addList({id == "liked" ? std::string("liked") : "playlist:" + id, toWide(meta.name)}, std::move(tracks));
        reply(remote::Response::json(200, dump(j)));
    }

    void playlist(const std::string& id, remote::Reply reply) {
        auto& lib = ctx().library;
        spotify::Api* api = source::activeApi();
        if (id == "liked" && !api) {   // the local Liked Songs
            catalog::Playlist meta;
            meta.name = toUtf8(tr(L"Beğenilen Şarkılar"));
            return sendPlaylist(reply, id, meta, lib.liked(), static_cast<int>(lib.liked().size()));
        }
        if (const auto* local = lib.playlist(id)) {
            const auto* tracks = lib.playlistTracks(id);
            std::vector<Track> list = tracks ? *tracks : std::vector<Track>{};
            const int n = static_cast<int>(list.size());
            return sendPlaylist(reply, id, *local, std::move(list), n);
        }
        if (!api || (id != "liked" && id.rfind("spotify:", 0) != 0)) return reply(refused(404, tr(L"Bir şeyler ters gitti")));
        struct Loaded {
            catalog::Playlist meta;
            std::vector<Track> tracks;
            int total = 0;
        };
        const bool liked = id == "liked";
        const std::string uri = liked ? std::string(spotify::Api::kLikedSongsUri) : id;
        async(Priority::High, life_.ref(),
              [api, uri, liked] {
                  Loaded l;
                  std::string owner;
                  int offset = 0;
                  for (int page = 0; page < kPlaylistPages; ++page) {
                      const bool first = page == 0 && !liked;
                      auto pg = api->playlistTracks(uri, offset, 100, {}, first ? &l.meta : nullptr, first ? &owner : nullptr);
                      if (page == 0) l.total = pg.total;
                      for (auto& t : pg.items) l.tracks.push_back(std::move(t));
                      const int next = pg.nextOffset >= 0 ? pg.nextOffset : offset + static_cast<int>(pg.items.size());
                      if (next <= offset || next >= pg.total) break;
                      offset = next;
                  }
                  if (l.meta.ownerName.empty()) l.meta.ownerName = owner;
                  return l;
              },
              [this, id, liked, reply](Result<Loaded> r) {
                  if (!r) return reply(refused(502, tr(L"Bir şeyler ters gitti")));
                  if (liked) r->meta.name = toUtf8(tr(L"Beğenilen Şarkılar"));
                  sendPlaylist(reply, id, r->meta, std::move(r->tracks), r->total);
              });
    }

    void artist(const std::string& id, remote::Reply reply) {
        spotify::Api* api = source::activeApi();
        if (source::isSpotifyId(id) && !api) return reply(refused(409, tr(L"Spotify bağlantısı yok")));
        async(Priority::High, life_.ref(), [api, id] { return source::artistPage(api, id); },
              [this, id, reply](Result<catalog::ArtistPage> r) {
                  if (!r) return reply(refused(502, tr(L"Bir şeyler ters gitti")));
                  const auto covers = coverMapper();
                  std::vector<Track> top(r->topTracks.begin(), r->topTracks.begin() + std::min<size_t>(10, r->topTracks.size()));
                  json albums = json::array();
                  for (const auto* group : {&r->albums, &r->singles})
                      for (const auto& a : *group)
                          if (albums.size() < 40) albums.push_back(remote::albumJson(a, covers));
                  json j = {{"artist", remote::artistJson(r->artist, covers, remote::kCoverPx)}, {"tracks", tracksJson(top)}, {"albums", albums}};
                  j["list"] = addList({"artist:" + id, toWide(r->artist.name)}, std::move(top));
                  reply(remote::Response::json(200, dump(j)));
              });
    }

    // A cover the app extracted itself (local files, downloads), by the handle the state / lists gave out.
    void cover(const std::string& handle, remote::Reply reply) {
        const auto it = covers_.find(handle);
        const std::wstring path = it == covers_.end() ? std::wstring{} : filePathOf(it->second);
        const char* mime = path.empty() ? nullptr : imageType(path);
        if (!mime) return reply(remote::Response::error(404));
        background(Priority::Normal, [path, mime, reply] {
            std::error_code ec;
            const auto size = std::filesystem::file_size(path, ec);
            std::string data;
            if (!ec && size > 0 && size <= kMaxCoverBytes) {
                std::ifstream f(path, std::ios::binary);
                data.resize(static_cast<size_t>(size));
                if (!f.read(data.data(), static_cast<std::streamsize>(data.size()))) data.clear();
            }
            if (data.empty()) return reply(remote::Response::error(404));
            remote::Response r;
            r.contentType = mime;
            r.body = std::move(data);
            r.headers.push_back({"Cache-Control", "private, max-age=3600"});
            reply(std::move(r));
        });
    }

    std::unique_ptr<remote::DeviceStore> store_;   // before router_ (which refers to it)
    std::unique_ptr<remote::Router> router_;
    std::unique_ptr<remote::Server> server_;
    Lifetime life_;              // the running server's posted work; renewed by stop()
    Lifetime hooks_;             // library / session subscriptions (for the whole run)
    std::string strings_;
    std::string bindAddress_ = "0.0.0.0";
    std::string failure_;
    bool portsInUse_ = false;
    std::vector<remote::LanAddress> addresses_;
    std::string loggedAddresses_;
    bool pushPending_ = false;
    std::atomic<bool> tickPending_{false};
    std::string lastState_, lastQueue_;
    std::deque<RemoteList> lists_;
    uint32_t nextList_ = 1;
    std::unordered_map<std::string, std::string> covers_;   // handle -> file:/// URL
    std::string lyricsFor_;
    std::shared_ptr<const lyrics::Lyrics> lyrics_;
    Lifetime lyricsLife_;
};

// ---- Ayarlar › TELEFONDAN KUMANDA --------------------------------------------------------------------------------------

// The QR code (dark modules on white in both themes, a 4-module quiet zone, every module a whole number of device
// pixels on pixel edges), the address and the pairing code, and the copy / renew buttons.
class PairingCard : public ui::Widget {
public:
    PairingCard(const std::string& url, const std::string& base, const std::string& code)
        : url_(url), title_(tr(L"Telefonunla tara"), type::sectionTitle), desc_(tr(L"QR kodu telefonunun kamerasıyla tara ya da "
                                                                                    L"adresi telefonundaki tarayıcıya yazıp "
                                                                                    L"bağlantı kodunu gir. Her telefon bir "
                                                                                    L"kez eşleşir."),
                                                                                 type::secondary),
          addressLabel_(toUpperTr(tr(L"Adres")), type::monoLabel), address_(toWide(base), type::monoDuration.withSize(14)),
          codeLabel_(toUpperTr(tr(L"Bağlantı kodu")), type::monoLabel), code_(toWide(code), type::monoDuration.withSize(14)) {
        hitTestVisible = false;
        gfx::TextOptions wrap;
        wrap.wrap = true;
        desc_.setOptions(wrap);
        try {
            const auto qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
            size_ = qr.getSize();
            modules_.resize(static_cast<size_t>(size_ * size_));
            for (int y = 0; y < size_; ++y)
                for (int x = 0; x < size_; ++x) modules_[static_cast<size_t>(y * size_ + x)] = qr.getModule(x, y);
        } catch (const std::exception& e) {
            ST_LOG_WARN(kTag, "QR code: {}", e.what());
        }
        copy_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Bağlantıyı kopyala"), "link");
        renew_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Yeni bağlantı kodu"), "refresh");
        copy_->onClick = [url = url_] {
            copyText(toWide(url));
            toast(tr(L"Bağlantı kopyalandı"));
        };
    }
    ui::Button* renewButton() const { return renew_; }

    float preferredHeight(float w) override {
        const float textH = 24 + 8 + desc_.measure(textWidth(w)).h + 16 + 2 * 44 + 16 + buttonsHeight(w);
        return std::max(kQr, textH) + 2 * kPad;
    }
    void layout() override {
        const Rect r = rect();
        const float x = kQr + 32;
        const float w = textWidth(r.w);
        const float cw = std::ceil(copy_->naturalWidth()), rw = std::ceil(renew_->naturalWidth());
        const float bottom = r.h - kPad;
        if (cw + 12 + rw <= w) {
            copy_->setRect({x, bottom - 40, cw, 40});
            renew_->setRect({x + cw + 12, bottom - 40, rw, 40});
        } else {   // a long translation: one under the other
            renew_->setRect({x, bottom - 40, rw, 40});
            copy_->setRect({x, bottom - 92, cw, 40});
        }
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = gfx::colors();
        paintQr(c, {r.x, r.y + kPad, kQr, kQr});
        const float x = r.x + kQr + 32;
        const float w = textWidth(r.w);
        float y = r.y + kPad;
        c.text(title_, {x, y, w, 24}, col.fgPrimary);
        y += 32;
        const float dh = desc_.measure(w).h;
        c.text(desc_, {x, y, w, dh + 2}, col.fgSecondary);
        y += dh + 16;
        c.text(addressLabel_, {x, y, w, 14}, col.fgTertiary);
        c.text(address_, {x, y + 16, w, 20}, col.fgPrimary);
        y += 44;
        c.text(codeLabel_, {x, y, w, 14}, col.fgTertiary);
        c.text(code_, {x, y + 16, w, 20}, col.fgPrimary);
        c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
        paintChildren(c);
    }

private:
    static constexpr float kQr = 184;   // the QR square (quiet zone included), before snapping to whole modules
    static constexpr float kPad = 20;
    static constexpr int kQuiet = 4;
    // QR colors: the white of tokens.light bg.overlay and the ink of tokens.dark bg.base, in both themes (scanners
    // want dark modules on a light ground).
    static constexpr gfx::Color kPaper = gfx::Color::rgb(0xFFFFFF);
    static constexpr gfx::Color kInk = gfx::Color::rgb(0x0F0D0B);

    float textWidth(float w) const { return std::max(160.f, w - kQr - 32); }
    float buttonsHeight(float w) {
        return std::ceil(copy_->naturalWidth()) + 12 + std::ceil(renew_->naturalWidth()) <= textWidth(w) ? 40.f : 92.f;
    }

    void paintQr(Canvas& c, Rect area) {
        if (size_ <= 0) return;
        const float s = c.scale();
        const int modules = size_ + 2 * kQuiet;
        const int px = std::max(1, static_cast<int>(std::floor(area.w * s / static_cast<float>(modules))));
        const float m = static_cast<float>(px) / s;   // one module in DIPs: a whole number of device pixels
        const float side = m * static_cast<float>(modules);
        // Snap the origin to a device pixel (the scroll offset and parents may leave fractions).
        D2D1_MATRIX_3X2_F t{};
        c.dc()->GetTransform(&t);
        const float cx = area.x + (area.w - side) * 0.5f, cy = area.y + (area.h - side) * 0.5f;
        const float x0 = std::round((cx + t._31) * s) / s - t._31;
        const float y0 = std::round((cy + t._32) * s) / s - t._32;
        c.fillRounded({x0, y0, side, side}, gfx::metrics::radiusSurface, kPaper);
        for (int y = 0; y < size_; ++y) {
            for (int x = 0; x < size_;) {
                if (!modules_[static_cast<size_t>(y * size_ + x)]) {
                    ++x;
                    continue;
                }
                int end = x;
                while (end < size_ && modules_[static_cast<size_t>(y * size_ + end)]) ++end;   // one rect per run
                c.fillRect({x0 + static_cast<float>(kQuiet + x) * m, y0 + static_cast<float>(kQuiet + y) * m,
                            static_cast<float>(end - x) * m, m},
                           kInk);
                x = end;
            }
        }
    }

    std::string url_;
    int size_ = 0;
    std::vector<bool> modules_;
    gfx::Text title_, desc_, addressLabel_, address_, codeLabel_, code_;
    ui::Button* copy_ = nullptr;
    ui::Button* renew_ = nullptr;
};

// The port field: applied with Enter or when it loses the focus.
class PortBox : public ui::TextBox {
public:
    PortBox() : ui::TextBox(ui::TextBox::Look::Field, std::to_wstring(kDefaultPort)) {}
    std::function<void(const std::wstring&)> onCommit;
    void onFocusChanged(bool focused) override {
        ui::TextBox::onFocusChanged(focused);
        if (!focused && onCommit) {
            const auto fn = onCommit;
            fn(text());
        }
    }
};

std::wstring addressLabel(const remote::LanAddress& a) {
    return a.adapter.empty() ? toWide(a.ip) : a.adapter;
}

} // namespace

void initRemote() { RemoteControl::get().init(); }

void buildRemoteSection(ui::Column* c, const std::function<void()>& rebuild) {
    auto& rc = RemoteControl::get();
    rc.onChanged = rebuild;   // a phone paired / connected / went away: the section follows
    auto& s = Settings::get();
    settingsToggle(c, tr(L"Telefondan kumanda"),
                   tr(L"Telefonunun tarayıcısından çalan şarkıyı, sırayı ve sesi yönet; bilgisayarında ara ve çal. Uygulama "
                      L"yüklemen gerekmez, telefon ve bilgisayar aynı ağda olmalı. İlk açılışta Windows Güvenlik Duvarı "
                      L"izin isteyebilir: özel ağlar için izin ver."),
                   s.remoteEnabled, [rebuild](bool v) {
                       Settings::get().remoteEnabled = v;
                       RemoteControl::get().apply();
                       rebuild();
                   });
    if (!s.remoteEnabled) return;

    if (!rc.running()) {
        auto* row = c->add<SettingRow>(
            tr(L"Telefondan kumanda başlatılamadı"),
            rc.portsInUse()
                ? i18n::format(tr(L"{} ve sonraki bağlantı noktaları kullanımda. Aşağıdan başka bir port seç."),
                               {std::to_wstring(s.remotePort)})
                : std::wstring(tr(L"Ağ dinlenemedi. Bir güvenlik yazılımının ShadeTube'u engellemediğinden emin ol.")));
        auto* again = row->control<ui::Button>(140.f, ui::ButtonKind::Secondary, tr(L"Tekrar dene"), "refresh");
        again->onClick = [rebuild] {
            RemoteControl::get().restart();
            rebuild();
        };
    } else {
        rc.refreshAddresses();
        const std::string url = rc.pairingUrl();
        if (url.empty()) {
            auto* row = c->add<SettingRow>(tr(L"Ağ bağlantısı bulunamadı"),
                                           tr(L"Bilgisayar Wi-Fi ya da Ethernet ile bir ağa bağlı olmalı. Bağlanınca "
                                              L"yeniden dene."));
            auto* again = row->control<ui::Button>(140.f, ui::ButtonKind::Secondary, tr(L"Tekrar dene"), "refresh");
            again->onClick = rebuild;
        } else {
            auto* card = c->add<PairingCard>(url, rc.baseUrl(), rc.store().pairingSecret());
            card->renewButton()->onClick = [rebuild] {
                RemoteControl::get().rotateSecret();
                toast(tr(L"Yeni bağlantı kodu oluşturuldu. Eşleşmiş telefonlar bağlı kalır."));
                rebuild();
            };
        }
        // The adapter: worth a row when there is a choice (or a pick to undo).
        const auto& addrs = rc.addresses();
        if (addrs.size() > 1 || !s.remoteAddress.empty()) {
            std::wstring current = tr(L"Otomatik");
            for (const auto& a : addrs)
                if (a.ip == s.remoteAddress) current = addressLabel(a);
            auto* row = c->add<SettingRow>(tr(L"Ağ bağdaştırıcısı"),
                                           tr(L"Telefonun bağlanacağı adres. Otomatik seçim sanal bağdaştırıcılar yerine "
                                              L"Wi-Fi ya da Ethernet'i tercih eder."));
            auto* pick = row->control<ui::Button>(220.f, ui::ButtonKind::Secondary, current, "chevron-down");
            pick->onClick = [pick, rebuild] {
                auto choose = [rebuild](std::string ip) {
                    return [rebuild, ip] {
                        Settings::get().remoteAddress = ip;
                        Settings::get().markDirty();
                        rebuild();
                    };
                };
                const std::string cur = Settings::get().remoteAddress;
                std::vector<ui::MenuItem> items;
                ui::MenuItem automatic{tr(L"Otomatik"), "wifi", L"", choose("")};
                automatic.checked = cur.empty();
                items.push_back(std::move(automatic));
                items.push_back(ui::MenuItem::sep());
                for (const auto& a : RemoteControl::get().addresses()) {
                    ui::MenuItem it{addressLabel(a), "", toWide(a.ip), choose(a.ip)};
                    it.checked = cur == a.ip;
                    items.push_back(std::move(it));
                }
                const Rect b = pick->toWindow(pick->rect());
                ui::Menu::open(ctx().window, {b.x, b.bottom() + 4}, std::move(items));
            };
        }
    }

    // The port.
    {
        auto* row = c->add<SettingRow>(
            tr(L"Bağlantı noktası"),
            i18n::format(tr(L"Varsayılan {}; kullanımdaysa sıradaki boş port seçilir. Adres ya da port değişince "
                            L"telefonlar QR kodu yeniden tarar."),
                         {std::to_wstring(kDefaultPort)}));
        auto* box = row->control<PortBox>(120.f);
        box->setText(std::to_wstring(s.remotePort));
        auto commit = [box, rebuild](const std::wstring& text) {
            const bool digits = !text.empty() && text.size() <= 5 &&
                                std::all_of(text.begin(), text.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; });
            const int v = digits ? std::stoi(text) : 0;
            if (v < 1024 || v > 65535) {
                box->setError(true);
                toast(tr(L"Geçersiz bağlantı noktası: 1024 ile 65535 arasında bir sayı olmalı."), true);
                return;
            }
            box->setError(false);
            if (v == Settings::get().remotePort) return;
            Settings::get().remotePort = v;
            Settings::get().markDirty();
            RemoteControl::get().restart();
            rebuild();
        };
        box->onSubmit = commit;
        box->onCommit = commit;
    }

    // The paired phones.
    const auto devices = rc.store().devices();
    c->add<SettingRow>(tr(L"Eşleşmiş telefonlar"),
                       devices.empty() ? std::wstring(tr(L"Henüz eşleşen telefon yok. QR kodu tarayan her telefon burada "
                                                         L"görünür."))
                                       : std::wstring(tr(L"Bağlantısını kestiğin telefon, QR kodu yeniden taramadan "
                                                         L"ShadeTube'u yönetemez.")));
    const auto online = rc.connectedDevices();
    for (const auto& d : devices) {
        const bool now = std::find(online.begin(), online.end(), d.id) != online.end();
        const std::wstring name = d.name.empty() ? std::wstring(tr(L"Bilinmeyen tarayıcı")) : toWide(d.name);
        auto* row = c->add<SettingRow>(name, now ? std::wstring(tr(L"Şimdi bağlı"))
                                                 : i18n::format(tr(L"Son bağlantı: {}"), {relativeTime(d.lastSeen)}));
        auto* cut = row->control<ui::Button>(150.f, ui::ButtonKind::Secondary, tr(L"Bağlantıyı kes"), "logout");
        cut->onClick = [d, name, rebuild] {
            RemoteControl::get().revoke(d);
            toast(i18n::format(tr(L"{} bağlantısı kesildi"), {name}));
            rebuild();
        };
    }
}

} // namespace st::app
