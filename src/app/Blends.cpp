// Blend in the app: the store, the exchange, the consent dialog and the actions (see Blends.h; the page and the cards
// are app/BlendPage.cpp).
#include "app/Blends.h"

#include "app/AppContext.h"
#include "app/JamCrypto.h"
#include "app/JamInvite.h"
#include "app/JamMqtt.h"
#include "app/JamProtocol.h"
#include "app/ListenStats.h"
#include "app/PageWidgets.h"
#include "app/Router.h"
#include "app/SmartListsPage.h"
#include "app/Together.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "ui/Controls.h"
#include "ui/Layout.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>

namespace st::app {

namespace blends {

namespace {

namespace fs = std::filesystem;
namespace mqtt = jam::mqtt;
using json = nlohmann::json;

constexpr const char* kTag = "blend";
constexpr int kFileVersion = 1;
constexpr int64_t kVisitMs = 10'000;              // a background visit to the broker
constexpr int64_t kWatchMs = 3 * 60'000;          // the page is open (renewed while it stays open)
constexpr int64_t kWaitForOthersMs = 30 * 60'000; // a new blend waits this long for someone to join, connected
constexpr int64_t kFirstSyncMs = 20'000;          // after startup
constexpr int64_t kSyncEveryMs = 6 * 3600'000;    // then
constexpr size_t kMaxBlends = 20;

struct Entry {
    std::string id;           // route id
    blend::Blend rec;
    jam::Invite inv;
    std::string base;         // topic base
    blend::Mix mix;
};

// A connection of one blend (several may be open at once).
struct Run {
    std::shared_ptr<mqtt::Client> client;
    std::shared_ptr<std::mutex> seqLock = std::make_shared<std::mutex>();
    std::shared_ptr<jam::SequenceCounter> seq = std::make_shared<jam::SequenceCounter>();
    bool connected = false;
    int64_t until = 0;          // steady ms: closed then
    bool stopWhenJoined = false;   // the new blend's wait: closes soon after someone shared
    bool ask = false;           // "ask" the members connected now to share their current taste
    bool asked = false;         // an "ask" from another member was shown this connection
    Lifetime life;
};

struct State {
    bool loaded = false;
    std::vector<std::string> pendingWatch;   // pages opened before blends.dat was read
    std::vector<Entry> entries;   // newest first
    std::map<std::string, Run> runs;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
    UINT_PTR timer = 0;
    int64_t nextAutoSync = 0;
    size_t autoCursor = 0;        // the next blend of a round of background visits
    bool autoRound = false;
    bool notifyPosted = false;
    uint64_t saveGen = 0;
    Lifetime life;
};

State& state() {
    static State s;
    return s;
}

fs::path storeFile() { return paths::appData() / L"blends.dat"; }

std::string routeId(const jam::Bytes& blendId) { return jam::toHex(jam::sha256(blendId)).substr(0, 16); }

std::string myName() {
    std::string n = together::displayName();
    return n.empty() ? std::string("ShadeTube") : n;
}

Entry* entryFor(const std::string& id) {
    for (auto& e : state().entries)
        if (e.id == id) return &e;
    return nullptr;
}

void notifyNow() {
    auto& s = state();
    std::erase_if(s.listeners, [](const auto& l) { return l.first.expired(); });
    const auto copy = s.listeners;
    for (const auto& [ref, fn] : copy)
        if (!ref.expired() && fn) fn();
}

void notify() {
    auto& s = state();
    if (s.notifyPosted) return;
    s.notifyPosted = true;
    Dispatcher::post([] {
        state().notifyPosted = false;
        notifyNow();
    });
}

// Dev (SHADETUBE_BLEND=...!, SHADETUBE_BLEND_INVITE_FILE): agree without the dialog, write a new blend's invite.
bool g_devAgree = false;
std::wstring g_devInviteFile;

void rebuild(Entry& e) {
    const uint64_t before = [&] {
        uint64_t h = 1469598103934665603ull;
        for (const auto& r : e.mix.rows)
            for (const char ch : r.track.id + "|" + std::to_string(r.from.size()) + ";") h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
        return h;
    }();
    e.mix = blend::build(blend::profilesOf(e.rec));
    // The order as a fingerprint: two members' logs show the same one when they built the same playlist.
    uint64_t h = 1469598103934665603ull;
    for (const auto& r : e.mix.rows)
        for (const char ch : r.track.id + "|" + std::to_string(r.from.size()) + ";") h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    if (h != before)
        ST_LOG_INFO(kTag, "{}: mix of {} member(s): {} songs ({} shared), match {} %, order {:016x}{}", e.id, e.mix.members.size(),
                    e.mix.rows.size(), e.mix.shared, e.mix.match, h,
                    e.mix.rows.empty() ? std::string() : ", first \"" + e.mix.rows.front().track.name + "\"");
}

// Covers for the first rows that have none (songs from an imported history): from the Stats page's artwork cache, a
// few looked up (one request at a time); the blend's cover and cards show them.
void enrichMix(const std::string& id, size_t lookups) {
    auto* e = entryFor(id);
    if (!e) return;
    std::vector<catalog::Track> heads;
    for (size_t i = 0; i < e->mix.rows.size() && i < 8; ++i)
        if (e->mix.rows[i].track.album.images.empty()) heads.push_back(e->mix.rows[i].track);
    if (heads.empty()) return;
    enrichTrackArtwork(heads, lookups, state().life.ref(), [id] {
        enrichMix(id, 0);
        notify();
    });
    for (const auto& t : heads)
        if (!t.album.images.empty())
            for (auto& r : e->mix.rows)
                if (r.track.id == t.id && r.track.album.images.empty()) r.track.album.images = t.album.images;
}

// ---- the file (DPAPI) ---------------------------------------------------------------------------------------------

void save() {
    auto& s = state();
    if (!s.loaded) return;
    json list = json::array();
    for (const auto& e : s.entries) list.push_back(blend::toJson(e.rec));
    auto text = std::make_shared<std::string>(json{{"v", kFileVersion}, {"blends", std::move(list)}}.dump(-1, ' ', false, json::error_handler_t::replace));
    const uint64_t gen = ++s.saveGen;
    static std::mutex writeLock;
    static uint64_t written = 0;
    background(Priority::Low, [text, gen] {
        std::lock_guard lk(writeLock);
        if (gen < written) return;   // a newer save already went
        written = gen;
        const auto sealed = jam::protect(jam::asBytes(*text), L"ShadeTube blends");
        SecureZeroMemory(text->data(), text->size());
        if (!sealed) {
            ST_LOG_WARN(kTag, "could not seal blends.dat");
            return;
        }
        fs::path tmp = storeFile();
        tmp += L".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            f.write(reinterpret_cast<const char*>(sealed->data()), static_cast<std::streamsize>(sealed->size()));
            if (!f) return;
        }
        std::error_code ec;
        fs::rename(tmp, storeFile(), ec);
    });
}

void addEntry(blend::Blend rec, bool front) {
    auto inv = jam::parseBlendInvite(rec.link);
    if (!inv) return;
    Entry e;
    e.id = routeId(inv->roomId);
    e.inv = *inv;
    e.base = jam::blendTopicBase(inv->roomId);
    e.rec = std::move(rec);
    rebuild(e);
    auto& list = state().entries;
    std::erase_if(list, [&](const Entry& x) { return x.id == e.id; });
    if (front) list.insert(list.begin(), std::move(e));
    else list.push_back(std::move(e));
}

// ---- the exchange ---------------------------------------------------------------------------------------------------

void startTimer();
void stopRun(const std::string& id);

std::string profileTopic(const Entry& e, const std::string& member) { return e.base + "/p/" + member; }

bool publishRecord(Entry& e, Run& r, const std::string& type, const json& body, bool retain, const std::string& topic) {
    if (!r.client || !r.connected) return false;
    jam::Envelope env;
    env.type = type;
    env.device = e.rec.me;
    {
        std::lock_guard lk(*r.seqLock);
        env.seq = r.seq->next();
    }
    env.body = body;
    return r.client->publish(topic, jam::sealEnvelope(e.inv.key, topic, env), retain);
}

void onConnected(const std::string& id) {
    auto* e = entryFor(id);
    auto it = state().runs.find(id);
    if (!e || it == state().runs.end()) return;
    Run& r = it->second;
    r.connected = true;
    // This PC's taste as it was shared (a broker that lost it gets it again); nothing new without the user.
    if (e->rec.mine) publishRecord(*e, r, "taste", blend::toJson(*e->rec.mine), true, profileTopic(*e, e->rec.me));
    if (r.ask) {
        r.ask = false;
        publishRecord(*e, r, "ask", {{"n", e->rec.mine ? e->rec.mine->name : myName()}}, false, e->base + "/x");
    }
    e->rec.syncedAt = nowUnix();
    save();
    notify();
}

void onRecord(const std::string& id, const mqtt::Publish& p) {
    auto* e = entryFor(id);
    if (!e || p.payload.empty()) return;
    auto env = jam::openEnvelope(e->inv.key, p.topic, p.payload);
    if (!env || env->device == e->rec.me) return;
    auto& rec = e->rec;
    const std::string prefix = e->base + "/p/";
    if (p.topic.rfind(prefix, 0) == 0) {
        const std::string member = p.topic.substr(prefix.size());
        if (env->device != member) return;   // a record only speaks for its own member
        bool changed = false;
        if (env->type == "taste") {
            auto prof = blend::profileFromJson(env->body);
            if (!prof || prof->member != member) return;
            auto known = std::find_if(rec.others.begin(), rec.others.end(), [&](const blend::Profile& x) { return x.member == member; });
            if (known == rec.others.end()) {
                if (rec.others.size() >= blend::kMaxMembers * 2) return;
                ST_LOG_INFO(kTag, "{}: {} shared their taste ({} songs)", id, prof->name, prof->tracks.size());
                const bool first = rec.others.empty() && std::find(rec.left.begin(), rec.left.end(), member) == rec.left.end();
                rec.others.push_back(std::move(*prof));
                changed = true;
                if (first && rec.mine)
                    toast(i18n::format(tr(L"{} Blend'e katıldı; ortak listeniz hazır"), {toWide(rec.others.back().name)}));
            } else if (prof->at >= known->at && !(*prof == *known)) {
                ST_LOG_INFO(kTag, "{}: {}'s taste changed", id, prof->name);
                *known = std::move(*prof);
                changed = true;
            }
            if (std::erase(rec.left, member) > 0) changed = true;
        } else if (env->type == "left") {
            if (std::find(rec.left.begin(), rec.left.end(), member) == rec.left.end()) {
                rec.left.push_back(member);
                changed = true;
                ST_LOG_INFO(kTag, "{}: a member left", id);
            }
        }
        if (!changed) return;
        rec.othersChangedAt = nowUnix();
        rebuild(*e);
        enrichMix(id, 8);
        save();
        notify();
        if (auto it = state().runs.find(id); it != state().runs.end() && it->second.stopWhenJoined && !rec.others.empty())
            it->second.until = std::min(it->second.until, steadyMs() + 3000);
        return;
    }
    if (p.topic == e->base + "/x" && env->type == "ask") {
        auto it = state().runs.find(id);
        if (it == state().runs.end() || it->second.asked || !rec.mine) return;
        it->second.asked = true;
        std::string who = env->body.is_object() && env->body.contains("n") && env->body["n"].is_string() ? env->body["n"].get<std::string>() : "";
        if (who.size() > 60) who.resize(60);
        ST_LOG_INFO(kTag, "{}: a member refreshed and asks for this PC's current taste", id);
        if (auto* w = ctx().window)
            ui::Toasts::show(w, i18n::format(tr(L"{} Blend'i yeniledi. Sen de güncel zevkini paylaşmak ister misin?"), {toWide(who)}),
                             ui::ToastKind::Info, tr(L"Paylaş"), [id] { refresh(id); });
    }
}

// Opens (or keeps open longer) the connection of a blend.
void visit(const std::string& id, int64_t forMs, bool ask = false, bool stopWhenJoined = false) {
    auto* e = entryFor(id);
    if (!e) return;
    auto& runs = state().runs;
    const int64_t until = steadyMs() + forMs;
    if (auto it = runs.find(id); it != runs.end()) {
        Run& r = it->second;
        r.until = std::max(r.until, until);
        r.stopWhenJoined = r.stopWhenJoined && stopWhenJoined;
        if (ask) {
            if (r.connected) publishRecord(*e, r, "ask", {{"n", e->rec.mine ? e->rec.mine->name : myName()}}, false, e->base + "/x");
            else r.ask = true;
        }
        if (r.connected && e->rec.mine) publishRecord(*e, r, "taste", blend::toJson(*e->rec.mine), true, profileTopic(*e, e->rec.me));
        return;
    }
    Run& r = runs[id];
    r.until = until;
    r.ask = ask;
    r.stopWhenJoined = stopWhenJoined;
    r.seq->newEpoch(static_cast<int64_t>(std::time(nullptr)));
    mqtt::Client::Events ev;
    const auto ref = r.life.ref();
    ev.onState = [ref, id](mqtt::Client::State st, const std::string& broker, const std::string&) {
        Dispatcher::post([ref, id, st, broker] {
            if (ref.expired()) return;
            auto it = state().runs.find(id);
            if (it == state().runs.end()) return;
            if (st == mqtt::Client::State::Connected) {
                ST_LOG_INFO(kTag, "{}: connected to {}", id, mqtt::brokerHost(broker));
                onConnected(id);
            } else {
                it->second.connected = false;
                notify();
            }
        });
    };
    ev.onMessage = [ref, id](const mqtt::Publish& p) {
        Dispatcher::post([ref, id, p] {
            if (!ref.expired()) onRecord(id, p);
        });
    };
    mqtt::Client::Config cfg;
    cfg.brokers = jam::brokerOrder(e->inv.broker, Settings::get().jamBroker);
    cfg.subscriptions = {e->base + "/#"};
    cfg.keepAliveSec = 20;
    r.client = std::make_shared<mqtt::Client>(std::move(ev));
    r.client->start(std::move(cfg));
    ST_LOG_INFO(kTag, "{}: connecting for {} s", id, forMs / 1000);
    startTimer();
    notify();
}

void stopRun(const std::string& id) {
    auto& runs = state().runs;
    auto it = runs.find(id);
    if (it == runs.end()) return;
    auto client = std::move(it->second.client);
    runs.erase(it);
    if (client)
        background(Priority::Normal, [client]() mutable {
            client->stop(std::chrono::milliseconds(800));   // what was queued (a "left" record) still goes
            client.reset();
        });
    notify();
}

void CALLBACK onTimer(HWND, UINT, UINT_PTR, DWORD) {
    auto& s = state();
    const int64_t t = steadyMs();
    std::vector<std::string> expired;
    for (const auto& [id, r] : s.runs)
        if (t >= r.until) expired.push_back(id);
    for (const auto& id : expired) {
        ST_LOG_INFO(kTag, "{}: closing the connection", id);
        stopRun(id);
    }
    // Background visits: one blend after the other.
    if (!s.autoRound && t >= s.nextAutoSync && !s.entries.empty()) {
        s.autoRound = true;
        s.autoCursor = 0;
    }
    if (s.autoRound) {
        if (s.autoCursor >= s.entries.size()) {
            s.autoRound = false;
            s.nextAutoSync = t + kSyncEveryMs;
        } else if (s.runs.size() < 3) {
            visit(s.entries[s.autoCursor++].id, kVisitMs);
        }
    }
}

void startTimer() {
    auto& s = state();
    if (!s.timer) s.timer = SetTimer(nullptr, 0, 1000, onTimer);
}

// ---- the consent dialog --------------------------------------------------------------------------------------------

// What `p` contains, in the words of the dialog: the artists, then the songs, all of them.
void addProfileList(ui::Widget* body, const blend::Profile& p) {
    auto* box = body->add<ui::Box>();
    auto* scroll = box->add<ui::ScrollView>();
    auto* col = scroll->setContent<ui::Column>(0.f);
    auto label = [col](const std::wstring& text, const gfx::TextStyle& style, ui::Tone tone) {
        auto* l = col->add<ui::Label>(text, style, tone);
        l->setWrap(true);
        l->setVAlign(gfx::VAlign::Top);
        return l;
    };
    label(toUpperTr(i18n::plural(L"{} sanatçı", static_cast<int64_t>(p.artists.size()))), gfx::type::monoLabel, ui::Tone::Tertiary);
    col->add<Spacer>(4.f);
    std::wstring names;
    for (const auto& a : p.artists) names += (names.empty() ? L"" : L", ") + toWide(a.name);
    label(names, gfx::type::secondary, ui::Tone::Secondary);
    col->add<Spacer>(14.f);
    label(toUpperTr(i18n::plural(L"{} şarkı", static_cast<int64_t>(p.tracks.size()))), gfx::type::monoLabel, ui::Tone::Tertiary);
    col->add<Spacer>(4.f);
    int n = 0;
    for (const auto& t : p.tracks) {
        std::wstring line = std::to_wstring(++n) + L". " + toWide(t.title);
        if (!t.artists.empty()) line += L" · " + toWide(t.artists[0].name);
        auto* l = label(line, gfx::type::secondary, ui::Tone::Secondary);
        l->setWrap(true, 2);
    }
    box->onPreferredHeight = [](float) { return 260.f; };
    box->onLayout = [scroll](ui::Box& b) { scroll->setRect({0, 0, b.rect().w, b.rect().h}); };
}

// Builds today's profile on a worker and shows it; `onAgree` gets it when the user agrees.
void withConsent(std::wstring title, std::wstring okLabel, std::function<void(blend::Profile)> onAgree) {
    auto& stats = listenStats();
    if (!stats.loaded()) {
        toast(tr(L"Dinleme geçmişin henüz yükleniyor; birazdan yeniden dene"), true);
        return;
    }
    auto tracks = std::make_shared<std::vector<ListenStats::Track>>(stats.tracks());
    auto plays = std::make_shared<std::vector<ListenStats::Play>>(stats.plays());
    async(
        Priority::High, state().life.ref(), [tracks, plays, now = nowUnix()] { return blend::makeProfile(*tracks, *plays, now); },
        [title = std::move(title), okLabel = std::move(okLabel), onAgree = std::move(onAgree)](Result<blend::Profile> r) {
            if (!r) {
                toast(tr(L"Dinleme geçmişin okunamadı"), true);
                return;
            }
            blend::Profile p = std::move(*r);
            if (!blend::usable(p)) {
                toast(i18n::format(tr(L"Blend için biraz daha dinlemen gerek: en az {} şarkı"), {std::to_wstring(blend::kMinProfileTracks)}), true);
                return;
            }
            p.name = myName();
            if (g_devAgree) {   // dev: what the dialog would have shown goes to the log instead
                ST_LOG_INFO(kTag, "dev: sharing without the dialog: {} songs, {} artists ({} days)", p.tracks.size(), p.artists.size(),
                            p.windowDays);
                onAgree(std::move(p));
                return;
            }
            const std::wstring period = p.windowDays > 0 ? std::wstring(tr(L"son altı ayda")) : std::wstring(tr(L"şimdiye kadar"));
            auto* d = ui::Dialog::open(
                ctx().window, title,
                i18n::format(tr(L"Blend'in diğer üyeleri {} en çok dinlediğin şu şarkıları ve sanatçıları \"{}\" adıyla görecek. "
                                L"Dinleme sayıların, ne zaman dinlediğin ve geçmişinin geri kalanı paylaşılmaz; paylaşılanlar "
                                L"davet bağlantısındaki anahtarla şifrelenir."),
                             {period, toWide(p.name)}),
                600);
            if (!d) return;
            addProfileList(d->body(), p);
            d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
            auto shared = std::make_shared<blend::Profile>(std::move(p));
            d->addButton(okLabel, ui::ButtonKind::Primary, [shared, onAgree] { onAgree(*shared); });
        });
}

std::wstring joinNames(const std::vector<std::wstring>& names) {
    if (names.empty()) return {};
    if (names.size() == 1) return names[0];
    if (names.size() <= 3) {
        std::wstring s;
        for (size_t i = 0; i < names.size(); ++i) s += (i ? L" + " : L"") + names[i];
        return s;
    }
    return i18n::format(tr(L"{}, {} ve {} kişi daha"), {names[0], names[1], std::to_wstring(names.size() - 2)});
}

Info infoOf(const Entry& e) {
    Info b;
    b.id = e.id;
    b.mix = e.mix;
    b.creator = e.rec.creator;
    b.link = e.rec.link;
    b.pending = !e.rec.mine;
    b.syncedAt = e.rec.syncedAt;
    b.connected = state().runs.count(e.id) && state().runs.at(e.id).connected;
    for (size_t i = 0; i < e.mix.members.size(); ++i)
        if (e.mix.members[i] == e.rec.me) b.me = static_cast<int>(i);
    const int others = static_cast<int>(e.mix.members.size()) - (b.me >= 0 ? 1 : 0);
    b.waiting = others <= 0;
    for (const auto& p : blend::profilesOf(e.rec)) b.updatedAt = std::max(b.updatedAt, p.at);
    for (const auto& p : e.rec.others)
        if (std::find(e.rec.left.begin(), e.rec.left.end(), p.member) != e.rec.left.end()) b.leftNames.push_back(toWide(p.name));
    std::vector<std::wstring> names;
    for (size_t i = 0; i < e.mix.names.size(); ++i) names.push_back(static_cast<int>(i) == b.me ? std::wstring(tr(L"Sen")) : toWide(e.mix.names[i]));
    if (b.waiting) b.title = tr(L"Sen + arkadaşın");
    else {
        // The others first, "Sen" last: "Ayşe + Sen".
        std::vector<std::wstring> ordered;
        for (size_t i = 0; i < names.size(); ++i)
            if (static_cast<int>(i) != b.me) ordered.push_back(names[i]);
        if (b.me >= 0) ordered.push_back(names[static_cast<size_t>(b.me)]);
        b.title = joinNames(ordered);
    }
    return b;
}

void showInvite(const std::string& id) {
    auto* e = entryFor(id);
    if (!e) return;
    auto* d = ui::Dialog::open(ctx().window, tr(L"Blend davet bağlantısı"),
                               tr(L"Bu bağlantıyı arkadaşına gönder. Bağlantıyı ShadeTube'da açınca kendi zevkini paylaşır ve ikiniz "
                                  L"de aynı ortak listeyi görürsünüz. Aynı anda çevrimiçi olmanız gerekmez."),
                               560);
    if (!d) return;
    auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, L"");
    box->setText(toWide(e->rec.link));
    const std::string link = e->rec.link;
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"Bağlantıyı kopyala"), ui::ButtonKind::Primary, [link] {
        copyText(toWide(link));
        toast(tr(L"Davet bağlantısı kopyalandı"));
    });
}

void navigateTo(const std::string& id) {
    if (ctx().router) ctx().router->navigate({RouteKind::SmartList, "blend:" + id});
}

void joinShared(const jam::Invite& inv, const std::string& link) {
    blend::Blend rec;
    rec.link = jam::formatBlendInvite(inv);
    if (rec.link.empty()) rec.link = link;
    rec.me = jam::toHex(jam::randomBytes(8));
    rec.createdAt = nowUnix();
    addEntry(std::move(rec), true);
    const std::string id = routeId(inv.roomId);
    save();
    ST_LOG_INFO(kTag, "{}: opened an invite", id);
    notify();
    visit(id, kWatchMs);   // the others' tastes arrive while the user looks at what they'd share
    navigateTo(id);
    share(id);
}

} // namespace

// =====================================================================================================================

std::vector<Info> all() {
    std::vector<Info> out;
    for (const auto& e : state().entries) out.push_back(infoOf(e));
    return out;
}

std::optional<Info> find(const std::string& id) {
    if (const auto* e = entryFor(id)) return infoOf(*e);
    return std::nullopt;
}

bool any() { return !state().entries.empty(); }

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { state().listeners.emplace_back(std::move(owner), std::move(fn)); }

void create() {
    if (state().entries.size() >= kMaxBlends) {
        toast(tr(L"En fazla 20 Blend olabilir; önce birinden ayrıl"), true);
        return;
    }
    withConsent(tr(L"Blend oluştur"), tr(L"Paylaş ve oluştur"), [](blend::Profile p) {
        const auto order = jam::brokerOrder("", Settings::get().jamBroker);
        jam::Invite inv = jam::newInvite(order.empty() ? std::string() : order.front());
        blend::Blend rec;
        rec.link = jam::formatBlendInvite(inv);
        if (rec.link.empty()) {
            toast(tr(L"Blend oluşturulamadı"), true);
            return;
        }
        rec.me = jam::toHex(jam::randomBytes(8));
        p.member = rec.me;
        rec.mine = std::move(p);
        rec.creator = true;
        rec.createdAt = nowUnix();
        addEntry(std::move(rec), true);
        const std::string id = routeId(inv.roomId);
        save();
        ST_LOG_INFO(kTag, "{}: created", id);
        enrichMix(id, 8);
        notify();
        visit(id, kWaitForOthersMs, false, true);
        navigateTo(id);
        if (!g_devInviteFile.empty()) {
            std::ofstream f(fs::path(g_devInviteFile), std::ios::binary | std::ios::trunc);
            f << entryFor(id)->rec.link;
        }
        if (!g_devAgree) showInvite(id);
    });
}

void promptJoin() {
    Dispatcher::post([] {
        auto* d = ui::Dialog::open(ctx().window, tr(L"Blend'e katıl"),
                                   tr(L"Arkadaşının gönderdiği Blend davet bağlantısını yapıştır."), 520);
        if (!d) return;
        auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, L"shadetube://blend/…");
        auto submit = [d, box] {
            const std::string text = toUtf8(box->text());
            if (!jam::parseBlendInvite(text)) {
                box->setError(true);
                toast(tr(L"Bu bir Blend davet bağlantısı değil"), true);
                return;
            }
            d->close();
            openInvite(text);
        };
        box->onSubmit = [submit](const std::wstring&) { submit(); };
        box->onChange = [box](const std::wstring&) { box->setError(false); };
        d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
        d->addButton(tr(L"Katıl"), ui::ButtonKind::Primary, submit, false);
        box->focus();
    });
}

bool openInvite(const std::string& link) {
    const auto inv = jam::parseBlendInvite(link);
    if (!inv) return false;
    const std::string id = routeId(inv->roomId);
    if (auto* e = entryFor(id)) {   // already in it: open it (and share, if that is still to do)
        navigateTo(id);
        if (!e->rec.mine) share(id);
        else visit(id, kWatchMs);
        return true;
    }
    if (state().entries.size() >= kMaxBlends) {
        toast(tr(L"En fazla 20 Blend olabilir; önce birinden ayrıl"), true);
        return true;
    }
    joinShared(*inv, link);
    return true;
}

void share(const std::string& id) {
    if (!entryFor(id)) return;
    withConsent(tr(L"Blend'e katıl"), tr(L"Paylaş ve katıl"), [id](blend::Profile p) {
        auto* e = entryFor(id);
        if (!e) return;
        p.member = e->rec.me;
        e->rec.mine = std::move(p);
        rebuild(*e);
        save();
        ST_LOG_INFO(kTag, "{}: shared this PC's taste ({} songs)", id, e->rec.mine->tracks.size());
        enrichMix(id, 8);
        visit(id, kWatchMs, true);
        notify();
    });
}

void refresh(const std::string& id) {
    if (!entryFor(id)) return;
    withConsent(tr(L"Blend'i yenile"), tr(L"Paylaş ve yenile"), [id](blend::Profile p) {
        auto* e = entryFor(id);
        if (!e) return;
        p.member = e->rec.me;
        e->rec.mine = std::move(p);
        rebuild(*e);
        save();
        ST_LOG_INFO(kTag, "{}: refreshed this PC's taste", id);
        enrichMix(id, 8);
        visit(id, kWatchMs, true);   // and ask the members connected now to share theirs
        notify();
        toast(tr(L"Blend yenilendi. Diğerlerinin zevki çevrimiçi olduklarında güncellenir."));
    });
}

void showShared(const std::string& id) {
    auto* e = entryFor(id);
    if (!e || !e->rec.mine) return;
    auto* d = ui::Dialog::open(ctx().window, tr(L"Bu Blend'de paylaştıkların"),
                               i18n::format(tr(L"\"{}\" adıyla paylaşıldı. Blend'i yenilediğinde güncel hâli, önce sana gösterilerek "
                                               L"bunun yerine geçer."),
                                            {toWide(e->rec.mine->name)}),
                               600);
    if (!d) return;
    addProfileList(d->body(), *e->rec.mine);
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
}

void copyInvite(const std::string& id) {
    if (auto* e = entryFor(id)) {
        copyText(toWide(e->rec.link));
        toast(tr(L"Davet bağlantısı kopyalandı"));
    }
}

namespace {
void leaveNow(const std::string& id);
}

void leave(const std::string& id) {
    if (!entryFor(id)) return;
    if (g_devAgree) {   // dev: without the question
        leaveNow(id);
        return;
    }
    ui::Dialog::confirm(ctx().window, tr(L"Blend'den ayrıl"),
                        tr(L"Bu Blend bu bilgisayardan silinir ve paylaştığın zevk diğerlerinin listesinden çıkar (onlar bir dahaki "
                           L"bağlanışlarında görür). Aynı davet bağlantısıyla yeniden katılabilirsin."),
                        tr(L"Ayrıl"), [id] { leaveNow(id); }, true);
}

namespace {

void leaveNow(const std::string& id) {
    auto* e = entryFor(id);
    if (!e) return;
    ST_LOG_INFO(kTag, "{}: leaving", id);
    // The "left" record replaces this member's retained taste; the connection closes once it went.
    const Entry copy = *e;
    auto& runs = state().runs;
    auto send = [copy](Run& r) {
        Entry tmp = copy;
        publishRecord(tmp, r, "left", json::object(), true, profileTopic(tmp, tmp.rec.me));
    };
    if (auto it = runs.find(id); it != runs.end() && it->second.connected) {
        send(it->second);
        stopRun(id);
    } else {
        stopRun(id);
        // A short connection of its own just for the record.
        auto seq = std::make_shared<jam::SequenceCounter>();
        seq->newEpoch(static_cast<int64_t>(std::time(nullptr)));
        const std::string topic = profileTopic(copy, copy.rec.me);
        jam::Envelope env{"left", copy.rec.me, seq->next(), json::object()};
        auto wire = jam::sealEnvelope(copy.inv.key, topic, env);
        mqtt::Client::Events ev;
        auto c = std::make_shared<std::shared_ptr<mqtt::Client>>();
        ev.onState = [c, topic, wire](mqtt::Client::State st, const std::string&, const std::string&) {
            if (st == mqtt::Client::State::Connected)
                if (auto cl = *c) cl->publish(topic, wire, true);
        };
        *c = std::make_shared<mqtt::Client>(std::move(ev));
        mqtt::Client::Config cfg;
        cfg.brokers = jam::brokerOrder(copy.inv.broker, Settings::get().jamBroker);
        (*c)->start(std::move(cfg));
        background(Priority::Low, [c] {
            Sleep(8000);   // connect + publish; then a clean close flushes it
            if (auto cl = *c) cl->stop(std::chrono::milliseconds(1000));
            c->reset();
        });
    }
    std::erase_if(state().entries, [&](const Entry& x) { return x.id == id; });
    save();
    notify();
    toast(tr(L"Blend'den ayrıldın"));
    if (ctx().router) ctx().router->navigate({RouteKind::Library});
}

} // namespace

void watch(const std::string& id) {
    if (!state().loaded) state().pendingWatch.push_back(id);
    else if (entryFor(id)) visit(id, kWatchMs);
}

bool loaded() { return state().loaded; }

std::wstring tasteLabel(const Info& b, const blend::Row& row) {
    if (row.from.size() >= 2) {
        if (row.from.size() == b.mix.members.size()) return toUpperTr(tr(L"Ortak"));
        std::vector<std::wstring> names;
        for (int i : row.from)
            names.push_back(i == b.me ? std::wstring(tr(L"Sen")) : toWide(b.mix.names[static_cast<size_t>(i)]));
        return toUpperTr(names[0] + L" + " + names[1]);
    }
    if (row.from.empty()) return {};
    const int i = row.from[0];
    return toUpperTr(i == b.me ? std::wstring(tr(L"Sen")) : toWide(b.mix.names[static_cast<size_t>(i)]));
}

std::wstring subtitle(const Info& b) {
    if (b.pending) return tr(L"Blend · katılmak için zevkini paylaş");
    if (b.waiting) return tr(L"Blend · arkadaşının katılması bekleniyor");
    if (b.mix.match >= 0) return i18n::format(tr(L"Blend · %{} uyum"), {std::to_wstring(b.mix.match)});
    return tr(L"Blend");
}

std::vector<PaletteCommand> paletteCommands() {
    std::vector<PaletteCommand> out;
    out.push_back({"blend:create", tr(L"Blend oluştur"), tr(L"Arkadaşınla ortak bir çalma listesi"), "users", [] { create(); }});
    out.push_back({"blend:join", tr(L"Blend davetiyle katıl…"), tr(L"Blend"), "link", [] { promptJoin(); }});
    for (const auto& b : all())
        out.push_back({"blend:" + b.id, b.title, subtitle(b), "playlist", [id = b.id] { navigateTo(id); }});
    return out;
}

} // namespace blends

void initBlends() {
    using namespace blends;
    auto& s = state();
    s.nextAutoSync = steadyMs() + kFirstSyncMs;
    async(
        Priority::Normal, s.life.ref(),
        [] {
            std::ifstream f(storeFile(), std::ios::binary);
            if (!f) return json();
            const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            auto plain = jam::unprotect(jam::asBytes(data));
            if (!plain) return json();
            json j = json::parse(plain->begin(), plain->end(), nullptr, false);
            SecureZeroMemory(plain->data(), plain->size());
            return j;
        },
        [](Result<json> r) {
            auto& st = state();
            st.loaded = true;
            notify();   // pages waiting for the file
            if (r && r->is_object() && r->value("v", 0) == kFileVersion)
                for (const auto& v : r->value("blends", json::array()))
                    if (auto rec = blend::blendFromJson(v)) addEntry(std::move(*rec), false);
            if (!st.entries.empty()) {
                ST_LOG_INFO(kTag, "{} blend(s)", st.entries.size());
                for (const auto& e : st.entries) enrichMix(e.id, 4);
                for (const auto& id : st.pendingWatch)
                    if (entryFor(id)) visit(id, kWatchMs);
                st.pendingWatch.clear();
                startTimer();
                notify();
            }
            // Dev: SHADETUBE_BLEND=create (consent dialog of a new blend) / <invite> (opens it, as a pasted link) /
            // refresh:<id> / leave:<id>; a "!" at the end agrees without the dialog. SHADETUBE_BLEND_INVITE_FILE receives a new blend's link. Once the
            // history is loaded (an SHADETUBE_IMPORT_HISTORY import may still be landing: a few seconds later).
            if (wchar_t env[1024]; GetEnvironmentVariableW(L"SHADETUBE_BLEND", env, 1024) > 0) {
                std::string v = toUtf8(env);
                if (!v.empty() && v.back() == '!') {
                    v.pop_back();
                    g_devAgree = true;
                }
                if (wchar_t file[1024]; GetEnvironmentVariableW(L"SHADETUBE_BLEND_INVITE_FILE", file, 1024) > 0) g_devInviteFile = file;
                SetTimer(nullptr, 0, 6000, [](HWND, UINT, UINT_PTR timerId, DWORD) {
                    KillTimer(nullptr, timerId);
                    wchar_t again[1024];
                    if (GetEnvironmentVariableW(L"SHADETUBE_BLEND", again, 1024) == 0) return;
                    std::string what = toUtf8(again);
                    if (!what.empty() && what.back() == '!') what.pop_back();
                    if (what == "create") create();
                    else if (what.rfind("refresh:", 0) == 0) refresh(what.substr(8));
                    else if (what.rfind("leave:", 0) == 0) leave(what.substr(6));
                    else if (!openInvite(what)) ST_LOG_WARN(kTag, "SHADETUBE_BLEND is not create / refresh:<id> / leave:<id> / a link");
                });
            }
        });
    ctx().persistHooks.push_back([] {
        auto& st = state();
        for (auto& [id, r] : st.runs)
            if (r.client) r.client->stop(std::chrono::milliseconds(300));
        st.runs.clear();
    });
}

} // namespace st::app
