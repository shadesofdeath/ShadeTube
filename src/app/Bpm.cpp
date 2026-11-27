// The tempo of songs in the app (see Bpm.h): the store's file, the background worker, lookups, settings rows.
#include "app/Bpm.h"

#include "app/AppContext.h"
#include "app/LocalLibrary.h"
#include "app/RuleLists.h"
#include "app/SettingsWidgets.h"
#include "audio/TagWriter.h"
#include "audio/Tempo.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "player/AudioCache.h"
#include "player/Player.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>

namespace st::app::bpm {

namespace {

constexpr int64_t kStartDelayMs = 60'000;     // nothing in the first minute (the startup's own work)
constexpr int64_t kTagGapMs = 3'000;          // between two batches of tag reads
constexpr size_t kTagBatch = 25;
constexpr int64_t kRescanMs = 5 * 60'000;     // candidates listed again at least this often
constexpr int64_t kSaveAfterMs = 30'000;
constexpr double kAnalyseSeconds = 70;

std::filesystem::path storeFile() { return paths::appData() / L"bpm.json"; }   // per PC (not per account)

// A file the worker may look at.
struct Candidate {
    std::wstring path;
    std::string mime;
    std::vector<std::string> ids;   // the first one decides "done" (Store::current)
    catalog::Track track;           // for the song key (name + first artist); empty for cached streams
    uint64_t stamp = 0;
    bool tags = false;              // a format whose tags we read (MP3 / M4A / FLAC)
};

struct Done {
    Candidate c;
    bool tagRead = false;
    int tagBpm = 0;
    audio::tempo::FileResult analysis;
    bool analysed = false;
    std::wstring key;
};

struct State {
    bool wired = false, loaded = false, loading = false;
    Store store;
    std::shared_ptr<const Store> snap;
    uint64_t snapRev = ~0ull;
    std::unordered_map<std::string, float> memo;   // track id -> answer, while the store's revision holds
    uint64_t memoRev = ~0ull;
    bool dirty = false;
    int64_t dirtySince = 0;
    bool saving = false;
    bool running = false;
    int64_t startedAt = 0;          // steadyMs() of init
    int64_t nextTagAt = 0, nextAnalysisAt = 0;
    std::vector<Candidate> tagQueue, analysisQueue;
    int64_t listedAt = 0;
    uint64_t listedLocalGen = ~0ull;
    size_t listedDownloads = SIZE_MAX, listedCached = SIZE_MAX;
    std::vector<std::pair<Lifetime::Ref, std::function<void()>>> listeners;
    bool notifyPosted = false;
    std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    std::vector<Entry> early;   // put before the file was read
    bool fast = false;          // dev: SHADETUBE_BPM_FAST
    Lifetime life;
};


State& state() {
    static State s;
    return s;
}

bool put(Entry e) {
    auto& s = state();
    if (!s.loaded) s.early.push_back(e);
    return s.store.put(std::move(e));
}

void notify() {
    auto& s = state();
    if (s.notifyPosted) return;
    s.notifyPosted = true;
    Dispatcher::post([] {
        auto& st = state();
        st.notifyPosted = false;
        std::erase_if(st.listeners, [](const auto& l) { return l.first.expired(); });
        const auto ls = st.listeners;
        for (const auto& [owner, fn] : ls)
            if (!owner.expired() && fn) fn();
    });
}

void markDirty() {
    auto& s = state();
    if (!s.dirty) s.dirtySince = steadyMs();
    s.dirty = true;
    notify();
}

void saveNow() {
    auto& s = state();
    if (!s.dirty || s.saving) return;
    s.dirty = false;
    s.saving = true;
    auto json = std::make_shared<nlohmann::json>(s.store.toJson());
    async(Priority::Low, s.life.ref(), [json] { return Store::save(*json, storeFile()); },
          [](Result<bool> r) {
              state().saving = false;
              if (!r || !*r) ST_LOG_WARN("bpm", "bpm.json could not be written");
          });
}

std::string extLower(const std::wstring& path) {
    const auto dot = path.rfind(L'.');
    if (dot == std::wstring::npos) return {};
    std::string e = toUtf8(path.substr(dot + 1));
    for (auto& ch : e) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return e;
}

uint64_t fileStamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(local::openablePath(path).c_str(), GetFileExInfoStandard, &fa)) return 0;
    const int64_t size = static_cast<int64_t>(fa.nFileSizeHigh) << 32 | fa.nFileSizeLow;
    const uint64_t mtime = static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32 | fa.ftLastWriteTime.dwLowDateTime;
    return stampOf(size, mtime);
}

// The files the worker hasn't looked at (or that changed): local files, then downloads, then the song cache.
void listCandidates() {
    auto& s = state();
    s.tagQueue.clear();
    s.analysisQueue.clear();
    s.listedAt = steadyMs();
    auto& lib = LocalLibrary::get();
    s.listedLocalGen = lib.generation();
    const auto& entries = lib.entries();
    const auto& tracks = lib.tracks();
    for (size_t i = 0; i < entries.size() && i < tracks.size(); ++i) {
        const auto& e = entries[i];
        const uint64_t stamp = stampOf(e.size, e.mtime);
        if (s.store.current(tracks[i].id, stamp)) continue;
        Candidate c;
        c.path = e.path;
        c.mime = player::Player::localMimeType(e.path);
        c.ids = {tracks[i].id};
        c.track = tracks[i];
        c.stamp = stamp;
        c.tags = audio::tags::writableExtension(e.path);
        (c.tags ? s.tagQueue : s.analysisQueue).push_back(std::move(c));
    }
    size_t downloads = 0;
    for (const auto& d : ctx().downloads.items()) {
        if (d.state != DlState::Done || d.filePath.empty()) continue;
        ++downloads;
        const uint64_t stamp = stampOf(d.sizeBytes, 0);
        if (s.store.current(d.track.id, stamp)) continue;
        Candidate c;
        c.path = toWide(d.filePath);
        c.mime = player::Player::localMimeType(c.path);
        c.ids = {d.track.id};
        c.track = d.track;
        c.stamp = stamp;
        c.tags = audio::tags::writableExtension(c.path);
        (c.tags ? s.tagQueue : s.analysisQueue).push_back(std::move(c));
    }
    s.listedDownloads = downloads;
    size_t cached = 0;
    if (auto* cache = ctx().audioCache; cache && cache->loaded()) {
        for (auto& [song, path] : cache->songs()) {
            ++cached;
            const uint64_t stamp = stampOf(song.length, song.hash);
            const std::string vid = "yt:" + song.videoId;
            if (s.store.current(vid, stamp)) continue;
            Candidate c;
            c.path = path;
            c.mime = song.mimeType;
            c.ids = {vid};
            for (const auto& id : song.trackIds) c.ids.push_back(id);
            c.stamp = stamp;
            s.analysisQueue.push_back(std::move(c));
        }
    }
    s.listedCached = cached;
    // Tags first come out of the tag queue: a file without a tag moves on to the analysis queue (front).
    std::reverse(s.tagQueue.begin(), s.tagQueue.end());
    std::reverse(s.analysisQueue.begin(), s.analysisQueue.end());
}

bool sourcesChanged() {
    auto& s = state();
    size_t downloads = 0;
    for (const auto& d : ctx().downloads.items())
        if (d.state == DlState::Done && !d.filePath.empty()) ++downloads;
    const size_t cached = ctx().audioCache && ctx().audioCache->loaded() ? static_cast<size_t>(ctx().audioCache->stats().count) : 0;
    return LocalLibrary::get().generation() != s.listedLocalGen || downloads != s.listedDownloads || cached != s.listedCached;
}

// Worker: one batch.
std::vector<Done> runJob(std::vector<Candidate> batch, bool analyse, std::shared_ptr<std::atomic<bool>> cancel) {
    const int before = GetThreadPriority(GetCurrentThread());
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);   // never ahead of the player's work
    std::vector<Done> out;
    for (auto& c : batch) {
        if (cancel->load()) break;
        Done d;
        if (!c.track.name.empty()) d.key = rules::songKey(c.track);
        if (c.tags) {
            const auto r = audio::tags::read(c.path);
            d.tagRead = r.ok;
            d.tagBpm = r.ok ? r.tags.bpm : 0;
        }
        if (analyse && d.tagBpm <= 0) {
            d.analysis = audio::tempo::analyzeFile(c.path, c.mime, kAnalyseSeconds, cancel.get());
            d.analysed = true;
        }
        d.c = std::move(c);
        out.push_back(std::move(d));
    }
    SetThreadPriority(GetCurrentThread(), before);
    return out;
}

void apply(std::vector<Done> results) {
    auto& s = state();
    bool changed = false;
    const int64_t now = nowUnix();
    for (auto& d : results) {
        if (d.tagBpm > 0) {
            changed |= s.store.put({static_cast<float>(d.tagBpm), Source::Tag, 1.f, now, d.c.stamp, d.c.ids, d.key});
            continue;
        }
        if (!d.analysed) {   // a tag read without a tempo: the file waits for the analysis
            d.c.tags = false;
            s.analysisQueue.push_back(std::move(d.c));
            continue;
        }
        const auto& a = d.analysis;
        if (!a.ok) {
            ST_LOG_INFO("bpm", "not analysed ({}): {}", a.error, toUtf8(std::filesystem::path(d.c.path).filename().wstring()));
            if (a.error == "cancelled") continue;
            // Kept as "no beat" so a file that can't be decoded isn't tried again until it changes.
        } else {
            ST_LOG_INFO("bpm", "{:.1f} BPM (confidence {:.2f}, {:.0f} s in {:.0f} + {:.0f} ms): {}", a.tempo.bpm, a.tempo.confidence,
                        a.seconds, a.decodeMs, a.analyzeMs, toUtf8(std::filesystem::path(d.c.path).filename().wstring()));
        }
        changed |= s.store.put({static_cast<float>(a.tempo.bpm), Source::Analysis, static_cast<float>(a.tempo.confidence), now,
                                d.c.stamp, d.c.ids, d.key});
    }
    if (changed) markDirty();
}

void startJob(std::vector<Candidate> batch, bool analyse) {
    auto& s = state();
    s.running = true;
    async(
        Priority::Low, s.life.ref(),
        [batch = std::move(batch), analyse, cancel = s.cancel]() mutable { return runJob(std::move(batch), analyse, cancel); },
        [](Result<std::vector<Done>> r) {
            auto& st = state();
            st.running = false;
            if (r) apply(std::move(*r));
        });
}

void tick() {
    auto& s = state();
    if (!s.loaded || s.running) return;
    const int64_t now = steadyMs();
    if (s.dirty && now - s.dirtySince >= kSaveAfterMs) saveNow();
    if (now - s.startedAt < kStartDelayMs) return;
    if (const auto* p = ctx().player; p && (p->status() == player::Status::Resolving || p->status() == player::Status::Buffering)) return;
    if ((s.tagQueue.empty() && s.analysisQueue.empty() && now - s.listedAt >= kRescanMs) || sourcesChanged() ||
        now - s.listedAt >= kRescanMs * 6)
        listCandidates();
    if (!s.tagQueue.empty() && now >= s.nextTagAt) {
        std::vector<Candidate> batch;
        while (!s.tagQueue.empty() && batch.size() < kTagBatch) {
            batch.push_back(std::move(s.tagQueue.back()));
            s.tagQueue.pop_back();
        }
        s.nextTagAt = now + kTagGapMs;
        startJob(std::move(batch), false);
        return;
    }
    if (!Settings::get().bpmAnalysis || s.analysisQueue.empty() || now < s.nextAnalysisAt) return;
    std::vector<Candidate> one;
    one.push_back(std::move(s.analysisQueue.back()));
    s.analysisQueue.pop_back();
    s.nextAnalysisAt = now + (s.fast ? 500 : kAnalysisGapMs);
    startJob(std::move(one), true);
}

} // namespace

void init() {
    auto& s = state();
    if (s.wired) return;
    s.wired = true;
    s.startedAt = steadyMs();
    // Dev: SHADETUBE_BPM_FAST=1 starts at once and analyses back to back (measurements, screenshots).
    if (GetEnvironmentVariableW(L"SHADETUBE_BPM_FAST", nullptr, 0) > 0) {
        s.startedAt -= kStartDelayMs;
        s.fast = true;
    }
    s.loading = true;
    async(Priority::Low, s.life.ref(), [] { return std::make_shared<Store>(Store::load(storeFile())); },
          [](Result<std::shared_ptr<Store>> r) {
              auto& st = state();
              st.loading = false;
              st.loaded = true;
              if (r && *r) {
                  st.store = std::move(**r);
                  for (auto& e : st.early) st.store.put(std::move(e));   // what was recorded meanwhile (a tag write)
                  ST_LOG_INFO("bpm", "{} songs with a tempo ({} ids)", st.store.known(), st.store.size());
              }
              st.early.clear();
              notify();
          });
    ctx().housekeepingHooks.push_back([] { tick(); });
    ctx().persistHooks.push_back([] {
        auto& st = state();
        st.cancel->store(true);
        if (st.dirty) Store::save(st.store.toJson(), storeFile());
        st.dirty = false;
    });
}

float of(const catalog::Track& t, const std::wstring& key) {
    auto& s = state();
    return s.store.of(t.id, t.videoId, key);
}

float of(const catalog::Track& t) {
    auto& s = state();
    if (t.id.empty() || s.store.size() == 0) return 0;
    if (s.memoRev != s.store.revision()) {
        s.memo.clear();
        s.memoRev = s.store.revision();
    }
    if (const auto it = s.memo.find(t.id); it != s.memo.end()) return it->second;
    float v = s.store.of(t.id, t.videoId, {});
    if (v <= 0 && !t.name.empty()) v = s.store.of({}, {}, rules::songKey(t));
    if (s.memo.size() > 50'000) s.memo.clear();
    s.memo.emplace(t.id, v);
    return v;
}

std::shared_ptr<const Store> snapshot() {
    auto& s = state();
    if (!s.snap || s.snapRev != s.store.revision()) {
        s.snap = std::make_shared<const Store>(s.store);
        s.snapRev = s.store.revision();
    }
    return s.snap;
}

uint64_t revision() { return state().store.revision(); }
bool loaded() { return state().loaded; }

void subscribe(Lifetime::Ref owner, std::function<void()> fn) { state().listeners.emplace_back(std::move(owner), std::move(fn)); }

void noteTag(const std::vector<std::string>& ids, const std::wstring& key, int value, uint64_t stamp) {
    auto& s = state();
    if (ids.empty()) return;
    if (value > 0) {
        if (put({static_cast<float>(value), Source::Tag, 1.f, nowUnix(), stamp, ids, key})) markDirty();
        return;
    }
    // The tag was removed: the file is looked at again (its analysis, if it has one, comes back with the next round).
    if (s.store.forget(ids)) {
        markDirty();
        s.listedAt = 0;
    }
}

Status status() {
    auto& s = state();
    Status st;
    st.known = s.store.known();
    st.fromTags = s.store.known(Source::Tag);
    st.fromAnalysis = s.store.known(Source::Analysis);
    st.waiting = s.tagQueue.size() + s.analysisQueue.size();
    st.running = s.running;
    return st;
}

void buildSettingsRows(ui::Column* c, std::function<void()> rebuild) {
    settingsToggle(c, tr(L"Tempo (BPM) analizi"),
                   tr(L"Etiketinde BPM olmayan yerel dosyaların, indirilenlerin ve önbellekteki şarkıların temposunu bu "
                      L"bilgisayarda, arka planda ölçer: dakikada birkaç şarkı, çalmayı yavaşlatmadan. Kapalıyken yalnızca "
                      L"dosyaların BPM etiketleri okunur."),
                   Settings::get().bpmAnalysis, [](bool v) { Settings::get().bpmAnalysis = v; });
    const Status st = status();
    std::wstring desc;
    if (st.known == 0)
        desc = tr(L"Henüz temposu bilinen şarkı yok.");
    else
        desc = i18n::format(tr(L"{}: {} etiketten, {} analizden."),
                            {i18n::plural(L"{} şarkının temposu biliniyor", static_cast<int64_t>(st.known)),
                             i18n::number(static_cast<int64_t>(st.fromTags)), i18n::number(static_cast<int64_t>(st.fromAnalysis))});
    if (st.waiting > 0) desc += L" " + i18n::plural(L"{} dosya sırada.", static_cast<int64_t>(st.waiting));
    c->add<SettingRow>(tr(L"Tempo bilgisi"), desc);
    (void)rebuild;
}

} // namespace st::app::bpm

namespace st::app {
void initTempo() { bpm::init(); }
} // namespace st::app
