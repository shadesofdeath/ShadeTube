#include "app/Downloads.h"

#include "app/AppContext.h"
#include "app/InternetRadio.h"
#include "app/SponsorBlock.h"
#include "audio/Downloader.h"
#include "catalog/TrackKind.h"
#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "ui/Window.h"
#include "youtube/MatchService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <random>

namespace st::app {

using json = nlohmann::json;
using namespace catalog;

namespace {
std::filesystem::path downloadsFile() { return paths::appData() / L"downloads.json"; }

std::wstring sanitizeName(const std::string& s) {
    std::wstring w = toWide(s);
    for (auto& c : w)
        if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) c = L'_';
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    if (w.empty()) w = L"_";
    if (w.size() > 80) w.resize(80);
    return w;
}

// SponsorBlock segments of the matched video to cut out of the MP3 (worker thread; blocking, k-anonymity
// hash-prefix lookup only). Never throws: a failed lookup just means an untrimmed download.
std::vector<std::pair<int64_t, int64_t>> sponsorCuts(const youtube::Match& m) {
    std::vector<std::pair<int64_t, int64_t>> cuts;
    if (m.videoId.empty()) return cuts;
    try {
        const auto segs = SponsorBlock::normalize(
            SponsorBlock::fetchSegments(m.videoId, SponsorBlock::defaultCategories(), /*hashPrefix=*/true),
            int64_t{m.durationSec} * 1000);
        for (const auto& seg : segs) cuts.emplace_back(seg.startMs, seg.endMs);
        ST_LOG_INFO("downloads", "sponsorblock: {} segment(s) to cut from {}", cuts.size(), m.videoId);
    } catch (const std::exception& e) {
        ST_LOG_WARN("downloads", "sponsorblock lookup failed, downloading untrimmed: {}", e.what());
    } catch (...) {
        ST_LOG_WARN("downloads", "sponsorblock lookup failed, downloading untrimmed");
    }
    return cuts;
}
} // namespace

std::wstring downloadTargetPath(const Track& t, bool mp3) {
    const auto& s = Settings::get();
    std::filesystem::path base = s.downloadsDir.empty() ? paths::downloadsDir() : std::filesystem::path(toWide(s.downloadsDir));
    const std::wstring artist = sanitizeName(t.artists.empty() ? "Bilinmeyen Sanatçı" : t.artists[0].name);
    const std::wstring title = sanitizeName(t.name.empty() ? t.id : t.name);
    std::filesystem::path dir = base / artist;
    if (!t.album.name.empty()) dir /= sanitizeName(t.album.name);
    return (dir / (title + (mp3 ? L".mp3" : L".m4a"))).wstring();
}

std::wstring cutSummary(int segments, int64_t ms) {
    if (segments <= 0) return {};
    // At least "1 sn": a segment that only grazed the end of the audio still removed something.
    const int64_t sec = std::max<int64_t>(1, (ms + 500) / 1000);
    std::wstring dur;
    if (sec < 60) dur = i18n::format(tr(L"{} sn"), sec);
    else if (sec % 60 == 0) dur = i18n::format(tr(L"{} dk"), sec / 60);
    else dur = i18n::format(tr(L"{} dk {} sn"), {std::to_wstring(sec / 60), std::to_wstring(sec % 60)});
    // "2 bölüm kesildi (1 dk 5 sn)": the count, then the total length removed.
    return i18n::plural(L"{} bölüm kesildi ({})", segments, {i18n::number(segments), dur});
}

// ===================================================================================================

void DownloadManager::notify() {
    std::erase_if(listeners_, [](const auto& l) { return l.first.expired(); });
    auto snapshot = listeners_;
    for (auto& [owner, fn] : snapshot)
        if (!owner.expired() && fn) fn();
}

void DownloadManager::touch() {
    dirty_ = true;
    notify();
}

DownloadItem* DownloadManager::find(const std::string& id) {
    for (auto& it : items_)
        if (it.track.id == id) return &it;
    return nullptr;
}
const DownloadItem* DownloadManager::find(const std::string& id) const {
    for (const auto& it : items_)
        if (it.track.id == id) return &it;
    return nullptr;
}
const DownloadItem* DownloadManager::item(const std::string& id) const { return find(id); }

bool DownloadManager::isDownloaded(const std::string& id) const {
    const auto* it = find(id);
    return it && it->state == DlState::Done;
}
bool DownloadManager::isActive(const std::string& id) const {
    const auto* it = find(id);
    return it && (it->state == DlState::Queued || it->state == DlState::Downloading);
}
int DownloadManager::activeCount() const {
    return static_cast<int>(std::count_if(items_.begin(), items_.end(),
                                          [](const DownloadItem& i) { return i.state == DlState::Queued || i.state == DlState::Downloading; }));
}
int64_t DownloadManager::totalBytes() const {
    int64_t n = 0;
    for (const auto& i : items_)
        if (i.state == DlState::Done) n += i.sizeBytes;
    return n;
}

void DownloadManager::enqueue(const Track& t, const std::string& collectionId) {
    // A live radio station has nothing to download; a podcast episode downloads as-is on the Podcastler pages.
    if (t.id.empty() || radio::isStationId(t.id) || catalog::isPodcastId(t.id)) return;
    if (auto* it = find(t.id)) {
        if (it->state == DlState::Failed) {
            it->state = DlState::Queued;   // retry
            it->error.clear();
        }
    } else {
        DownloadItem d;
        d.track = t;
        d.state = DlState::Queued;
        d.addedAt = nowUnix();
        items_.insert(items_.begin(), std::move(d));
    }
    if (!collectionId.empty()) addToCollection(collectionId, t.id);
    touch();
    startNext();
}

void DownloadManager::enqueue(const std::vector<Track>& tracks, const std::string& collectionId) {
    for (const auto& t : tracks) enqueue(t, collectionId);
}

void DownloadManager::cancel(const std::string& id) {
    if (id == activeId_ && cancelFlag_) cancelFlag_->store(true);
    // Queued (not active) → just drop it.
    if (auto* it = find(id); it && it->state == DlState::Queued) {
        std::erase_if(items_, [&](const DownloadItem& d) { return d.track.id == id; });
        touch();
    }
}

void DownloadManager::remove(const std::string& id) {
    if (id == activeId_ && cancelFlag_) cancelFlag_->store(true);
    if (const auto* it = find(id); it && !it->filePath.empty()) {
        std::error_code ec;
        std::filesystem::remove(toWide(it->filePath), ec);
    }
    std::erase_if(items_, [&](const DownloadItem& d) { return d.track.id == id; });
    for (auto& c : collections_) std::erase(c.trackIds, id);
    touch();
}

void DownloadManager::startNext() {
    if (!activeId_.empty()) return;   // one at a time
    DownloadItem* job = nullptr;
    for (auto& it : items_)
        if (it.state == DlState::Queued) {
            job = &it;
            break;
        }
    if (!job) return;

    activeId_ = job->track.id;
    job->state = DlState::Downloading;
    job->progress = 0;
    cancelFlag_ = std::make_shared<std::atomic<bool>>(false);
    notify();

    const Track track = job->track;
    const std::string trackId = track.id;
    auto* matcher = ctx().matcher;
    const int kbps = Settings::get().downloadMp3Kbps;
    const bool mp3 = kbps > 0;
    const bool trim = mp3 && Settings::get().sponsorBlockEnabled;   // passthrough can't be cut (no re-encode)
    const bool lowQ = Settings::get().quality == AudioQuality::Normal;
    const std::wstring target = downloadTargetPath(track, mp3);
    auto cancel = cancelFlag_;

    struct Out {
        audio::DownloadStatus status = audio::DownloadStatus::NetworkError;
        std::wstring path;
        int64_t size = 0;
        std::string error;
        int cutSegments = 0;
        int64_t cutMs = 0;
    };

    async(
        Priority::Low, life_.ref(),
        [matcher, track, kbps, mp3, trim, lowQ, target, cancel, trackId]() -> Out {
            Out o;
            o.path = target;
            try {
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(target).parent_path(), ec);
                const auto resolved = matcher->resolve(track, /*allowWebm=*/mp3, lowQ);

                audio::DownloadRequest req;
                req.url = resolved.stream.url;
                req.mimeType = resolved.stream.mimeType;
                req.contentLength = resolved.stream.contentLength;
                req.outPath = target;
                req.mp3Kbps = kbps;
                if (trim && !cancel->load()) req.cutMs = sponsorCuts(resolved.match);
                req.title = track.name;
                req.artist = track.artistLine();
                req.album = track.album.name;
                req.year = track.album.name.empty() ? std::string{} : std::string{};
                if (const auto* img = pickImage(track.album.images, 300)) {
                    try {
                        const auto resp = http::get(img->url);
                        if (resp.isSuccessStatusCode() && !resp.body.empty())
                            req.coverJpeg.assign(resp.body.begin(), resp.body.end());
                    } catch (...) {
                    }
                }
                auto progressCb = [trackId](float f) {
                    Dispatcher::post([trackId, f] {
                        auto& dm = ctx().downloads;
                        // find + update without exposing internals: use public path
                        dm.reportProgress(trackId, f);
                    });
                };
                audio::DownloadStats stats;
                o.status = audio::downloadTrack(req, progressCb, *cancel, &stats);
                if (o.status == audio::DownloadStatus::Ok) {
                    std::error_code ec2;
                    o.size = static_cast<int64_t>(std::filesystem::file_size(target, ec2));
                    o.cutSegments = stats.segmentsCut;
                    o.cutMs = stats.cutMs;
                }
            } catch (const std::exception& e) {
                o.status = audio::DownloadStatus::NetworkError;
                o.error = e.what();
            }
            return o;
        },
        [this, trackId, cancel](Result<Out> r) {
            activeId_.clear();
            cancelFlag_.reset();
            auto* it = find(trackId);
            const bool wasCancelled = cancel && cancel->load();
            if (it) {
                if (wasCancelled) {
                    std::erase_if(items_, [&](const DownloadItem& d) { return d.track.id == trackId; });
                    for (auto& c : collections_) std::erase(c.trackIds, trackId);
                } else if (r && r->status == audio::DownloadStatus::Ok) {
                    it->state = DlState::Done;
                    it->filePath = toUtf8(r->path);
                    it->sizeBytes = r->size;
                    it->progress = 1.f;
                    it->cutSegments = r->cutSegments;
                    it->cutMs = r->cutMs;
                    if (r->cutSegments > 0)
                        toast(i18n::format(tr(L"\"{}\" indirildi · {}"),
                                           {toWide(it->track.name), cutSummary(r->cutSegments, r->cutMs)}));
                } else {
                    it->state = DlState::Failed;
                    it->error = r ? (r->error.empty() ? audio::toString(r->status) : r->error) : "hata";
                    ST_LOG_WARN("downloads", "\"{}\" failed: {}", it->track.name, it->error);
                }
            }
            touch();
            startNext();   // pump the next queued item
        });
}

void DownloadManager::reportProgress(const std::string& id, float f) {
    if (auto* it = find(id); it && it->state == DlState::Downloading) {
        if (f - it->progress >= 0.01f || f >= 1.f) {
            it->progress = f;
            notify();
            if (ctx().window) ctx().window->invalidate();
        }
    }
}

void DownloadManager::tick() { startNext(); }

// ---- Collections ----------------------------------------------------------------------------------

const Collection* DownloadManager::collection(const std::string& id) const {
    for (const auto& c : collections_)
        if (c.id == id) return &c;
    return nullptr;
}

std::string DownloadManager::createCollection(const std::wstring& name) {
    static std::mt19937_64 rng(std::random_device{}());
    char id[40];
    snprintf(id, sizeof id, "col:%016llx", static_cast<unsigned long long>(rng()));
    Collection c;
    c.id = id;
    c.name = toUtf8(name.empty() ? tr(L"Yeni klasör") : name);
    c.createdAt = nowUnix();
    collections_.insert(collections_.begin(), c);
    touch();
    return c.id;
}

void DownloadManager::renameCollection(const std::string& id, const std::wstring& name) {
    for (auto& c : collections_)
        if (c.id == id) c.name = toUtf8(name);
    touch();
}

void DownloadManager::deleteCollection(const std::string& id) {
    std::erase_if(collections_, [&](const Collection& c) { return c.id == id; });
    touch();
}

void DownloadManager::addToCollection(const std::string& id, const std::string& trackId) {
    for (auto& c : collections_)
        if (c.id == id && std::find(c.trackIds.begin(), c.trackIds.end(), trackId) == c.trackIds.end()) {
            c.trackIds.push_back(trackId);
            touch();
        }
}

void DownloadManager::addToCollection(const std::string& id, const std::vector<Track>& tracks) {
    for (const auto& t : tracks) {
        if (!isDownloaded(t.id) && !isActive(t.id)) enqueue(t, id);
        else addToCollection(id, t.id);
    }
}

void DownloadManager::removeFromCollection(const std::string& id, const std::string& trackId) {
    for (auto& c : collections_)
        if (c.id == id) std::erase(c.trackIds, trackId);
    touch();
}

std::vector<Track> DownloadManager::collectionTracks(const std::string& id) const {
    std::vector<Track> out;
    const auto* c = collection(id);
    if (!c) return out;
    for (const auto& tid : c->trackIds)
        if (const auto* it = find(tid)) out.push_back(it->track);
    return out;
}

// ---- Persistence ----------------------------------------------------------------------------------

void DownloadManager::load() {
    std::ifstream f(downloadsFile());
    if (!f) return;
    json j = json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    for (const auto& e : j.value("items", json::array())) {
        DownloadItem d;
        d.track = trackFromJson(e.value("t", json::object()));
        if (d.track.id.empty()) continue;
        const std::string st = e.value("s", "done");
        d.state = st == "failed" ? DlState::Failed : DlState::Done;   // queued/downloading aren't persisted
        d.filePath = e.value("f", "");
        d.sizeBytes = e.value("z", int64_t{0});
        d.addedAt = e.value("at", int64_t{0});
        d.error = e.value("e", "");
        d.cutSegments = std::max(0, e.value("cs", 0));   // absent in older files: not trimmed
        d.cutMs = std::max<int64_t>(0, e.value("cm", int64_t{0}));
        // A "done" record whose file vanished becomes a fresh (re-queueable) failure.
        if (d.state == DlState::Done && (d.filePath.empty() || !std::filesystem::exists(toWide(d.filePath)))) continue;
        if (d.state == DlState::Done) d.progress = 1.f;
        items_.push_back(std::move(d));
    }
    for (const auto& e : j.value("collections", json::array())) {
        Collection c;
        c.id = e.value("id", "");
        c.name = e.value("n", "");
        c.createdAt = e.value("at", int64_t{0});
        if (c.id.empty()) continue;
        for (const auto& tid : e.value("tracks", json::array()))
            if (tid.is_string()) c.trackIds.push_back(tid.get<std::string>());
        collections_.push_back(std::move(c));
    }
    ST_LOG_INFO("downloads", "loaded: {} items, {} collections", items_.size(), collections_.size());
}

void DownloadManager::saveIfDirty() {
    if (!dirty_) return;
    dirty_ = false;
    json items = json::array();
    for (const auto& d : items_) {
        if (d.state != DlState::Done && d.state != DlState::Failed) continue;   // don't persist transient states
        json e = {{"t", toJson(d.track)},
                  {"s", d.state == DlState::Failed ? "failed" : "done"},
                  {"f", d.filePath},
                  {"z", d.sizeBytes},
                  {"at", d.addedAt},
                  {"e", d.error}};
        if (d.cutSegments > 0) {
            e["cs"] = d.cutSegments;
            e["cm"] = d.cutMs;
        }
        items.push_back(std::move(e));
    }
    json cols = json::array();
    for (const auto& c : collections_)
        cols.push_back({{"id", c.id}, {"n", c.name}, {"at", c.createdAt}, {"tracks", c.trackIds}});

    json j;
    j["version"] = 1;
    j["items"] = std::move(items);
    j["collections"] = std::move(cols);

    auto tmp = downloadsFile();
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << j.dump();
    }
    std::error_code ec;
    std::filesystem::rename(tmp, downloadsFile(), ec);
}

} // namespace st::app
