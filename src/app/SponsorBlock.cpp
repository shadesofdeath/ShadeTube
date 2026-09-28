#include "app/SponsorBlock.h"

#include "core/Http.h"
#include "core/Log.h"

#include <windows.h>
#include <bcrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <stdexcept>
#include <utility>

namespace st::app {

namespace {

constexpr const char* kApi = "https://sponsor.ajay.app/api/skipSegments";

constexpr int64_t kMinSegmentMs = 1000;         // shorter: the seek's rebuffer blip costs more than it saves
constexpr int64_t kMergeGapMs = 500;            // neighbours this close are skipped in one seek
constexpr int64_t kDurationToleranceMs = 3000;  // videoDuration vs. real length: more = outdated submission
constexpr int64_t kRearmLeadMs = 250;           // playhead seen this far before a segment -> re-arm it
constexpr int64_t kRestartWindowMs = 1000;      // backward jump landing within a segment's first second
constexpr int64_t kBackJumpMs = 1500;           // position went back by more than this -> user seek/restart
constexpr int64_t kMinRemainingMs = 500;        // don't seek for the last half second of a segment
constexpr int64_t kRetryAfterMs = 30000;        // failed lookup: retry from check() after this long
constexpr size_t kCacheSize = 32;

int64_t steadyNowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

std::string categoriesKey(const std::vector<std::string>& cats) {
    std::string key = "c:";   // never empty, so "" can mean "not requested yet"
    for (const auto& c : cats) {
        key += c;
        key.push_back(',');
    }
    return key;
}

int64_t secondsToMs(double s) { return static_cast<int64_t>(std::llround(s * 1000.0)); }

std::string stringField(const nlohmann::json& obj, const char* name) {
    const auto it = obj.find(name);
    return it != obj.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

// One element of the direct response / of a hash-bucket entry's "segments".
void parseSegment(const nlohmann::json& e, std::vector<SponsorBlock::Segment>& out) {
    if (!e.is_object()) return;
    // actionType is always present today; very old responses omitted it and meant "skip".
    const auto action = e.find("actionType");
    if (action != e.end() && !(action->is_string() && action->get_ref<const std::string&>() == "skip")) return;
    const auto seg = e.find("segment");
    if (seg == e.end() || !seg->is_array() || seg->size() < 2 || !(*seg)[0].is_number() || !(*seg)[1].is_number())
        return;
    SponsorBlock::Segment s;
    s.startMs = secondsToMs((*seg)[0].get<double>());
    s.endMs = secondsToMs((*seg)[1].get<double>());
    s.category = stringField(e, "category");
    if (const auto d = e.find("videoDuration"); d != e.end() && d->is_number() && d->get<double>() > 0)
        s.videoDurationMs = secondsToMs(d->get<double>());
    out.push_back(std::move(s));
}

std::vector<SponsorBlock::Segment> keepCategories(std::vector<SponsorBlock::Segment> segs,
                                                  const std::vector<std::string>& categories) {
    std::erase_if(segs, [&](const SponsorBlock::Segment& s) {
        return std::find(categories.begin(), categories.end(), s.category) == categories.end();
    });
    return segs;
}

} // namespace

SponsorBlock::SponsorBlock() : categories(defaultCategories()) {}
SponsorBlock::~SponsorBlock() = default;   // life_ dies with us: an in-flight lookup's result is dropped

std::vector<std::string> SponsorBlock::defaultCategories() {
    return {"music_offtopic", "sponsor", "selfpromo", "interaction"};
}

// --- UI-thread API ----------------------------------------------------------------------------------------

void SponsorBlock::setVideo(const std::string& videoId, int64_t videoDurationMs) {
    if (!videoId.empty() && videoId == videoId_) return;   // same video: keep segments + armed state
    life_.renew();                                          // drop the previous video's lookup result
    videoId_ = videoId;
    videoDurationMs_ = std::max<int64_t>(0, videoDurationMs);
    requestKey_.clear();
    segments_.clear();
    armed_.clear();
    lastSkipped_ = -1;
    lastPos_ = -1;
    state_ = State::Idle;
    ensureFetched();
}

void SponsorBlock::ensureFetched() {
    if (!enabled || videoId_.empty()) return;
    const std::string cats = categoriesKey(categories);
    if (cats == requestKey_) return;   // loaded, in flight or failed for exactly this configuration
    life_.renew();
    requestKey_ = cats;
    segments_.clear();
    armed_.clear();
    lastSkipped_ = -1;
    if (categories.empty()) {
        state_ = State::Ready;
        return;
    }
    std::string key = videoId_ + '\n' + cats;
    if (const auto* hit = findCache(key)) {
        apply(*hit);
        state_ = State::Ready;
        return;
    }
    state_ = State::Loading;
    st::async(
        Priority::High, life_.ref(),
        [id = videoId_, wanted = categories] { return fetchSegments(id, wanted, true); },
        [this, id = videoId_, key = std::move(key)](Result<std::vector<Segment>> r) {
            if (id != videoId_) return;   // belt and braces: renew() already guards this
            if (!r) {
                state_ = State::Failed;
                failedAtMs_ = steadyNowMs();
                ST_LOG_WARN("sponsorblock", "lookup failed for {}: {}", id, r.errorMessage());
                return;
            }
            apply(*r);
            storeCache(key, std::move(*r));
            state_ = State::Ready;
            ST_LOG_INFO("sponsorblock", "{}: {} segment(s) to skip", id, segments_.size());
        });
}

void SponsorBlock::apply(const std::vector<Segment>& raw) {
    segments_ = normalize(raw, videoDurationMs_);
    armed_.assign(segments_.size(), 1);
    lastSkipped_ = -1;
}

void SponsorBlock::setSegments(std::vector<Segment> raw) {
    life_.renew();
    requestKey_ = categoriesKey(categories);   // counts as loaded: check() won't start a lookup
    apply(raw);
    lastPos_ = -1;
    state_ = State::Ready;
}

std::optional<int64_t> SponsorBlock::check(int64_t positionMs) {
    const int64_t last = lastPos_;
    lastPos_ = positionMs;
    if (!enabled) return std::nullopt;
    if (state_ == State::Failed && steadyNowMs() - failedAtMs_ >= kRetryAfterMs) requestKey_.clear();
    ensureFetched();
    if (segments_.empty()) return std::nullopt;

    // Re-arm skipped segments the playhead has come back in front of (seek back, restart, repeat-one).
    const bool jumpedBack = last >= 0 && positionMs < last - kBackJumpMs;
    for (size_t i = 0; i < segments_.size(); ++i) {
        if (armed_[i]) continue;
        const Segment& s = segments_[i];
        if (positionMs < s.startMs - kRearmLeadMs || (jumpedBack && positionMs <= s.startMs + kRestartWindowMs))
            armed_[i] = 1;
    }
    // 0 is what Player reports while tracks switch (possibly for the NEXT video): never act on it.
    if (positionMs <= 0) return std::nullopt;

    for (size_t i = 0; i < segments_.size(); ++i) {
        const Segment& s = segments_[i];
        if (positionMs < s.startMs) break;   // sorted and non-overlapping
        if (positionMs >= s.endMs || !armed_[i]) continue;
        armed_[i] = 0;
        if (s.endMs - positionMs < kMinRemainingMs) return std::nullopt;
        lastSkipped_ = static_cast<int>(i);
        ST_LOG_INFO("sponsorblock", "skipping {} {} -> {} ms", s.category, positionMs, s.endMs);
        return s.endMs;
    }
    return std::nullopt;
}

const SponsorBlock::Segment* SponsorBlock::lastSkipped() const {
    return lastSkipped_ >= 0 && lastSkipped_ < static_cast<int>(segments_.size()) ? &segments_[lastSkipped_] : nullptr;
}

const std::vector<SponsorBlock::Segment>* SponsorBlock::findCache(const std::string& key) const {
    for (const auto& e : cache_)
        if (e.key == key) return &e.raw;
    return nullptr;
}

void SponsorBlock::storeCache(std::string key, std::vector<Segment> raw) {
    std::erase_if(cache_, [&](const CacheEntry& e) { return e.key == key; });
    cache_.push_back({std::move(key), std::move(raw)});
    if (cache_.size() > kCacheSize) cache_.erase(cache_.begin());
}

// --- pure helpers / network ---------------------------------------------------------------------------------

std::vector<SponsorBlock::Segment> SponsorBlock::parseResponse(std::string_view json, const std::string& videoId) {
    const auto j = nlohmann::json::parse(json.begin(), json.end());   // throws on malformed input
    if (!j.is_array()) throw std::runtime_error("SponsorBlock: unexpected response shape");
    std::vector<Segment> out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        if (const auto v = e.find("videoID"); v != e.end()) {   // hash-prefix shape: one entry per video
            if (!v->is_string() || v->get_ref<const std::string&>() != videoId) continue;
            if (const auto segs = e.find("segments"); segs != e.end() && segs->is_array())
                for (const auto& s : *segs) parseSegment(s, out);
            continue;
        }
        parseSegment(e, out);   // direct shape
    }
    return out;
}

std::vector<SponsorBlock::Segment> SponsorBlock::normalize(std::vector<Segment> segments, int64_t videoDurationMs) {
    std::vector<Segment> valid;
    valid.reserve(segments.size());
    for (auto& s : segments) {
        s.startMs = std::max<int64_t>(0, s.startMs);
        if (s.endMs <= s.startMs) continue;
        if (videoDurationMs > 0 && s.videoDurationMs > 0 &&
            std::llabs(s.videoDurationMs - videoDurationMs) > kDurationToleranceMs)
            continue;   // submitted for a different cut of the video: its timestamps can't be trusted
        valid.push_back(std::move(s));
    }
    std::sort(valid.begin(), valid.end(), [](const Segment& a, const Segment& b) {
        return a.startMs != b.startMs ? a.startMs < b.startMs : a.endMs > b.endMs;
    });
    std::vector<Segment> merged;
    for (auto& s : valid) {
        if (!merged.empty() && s.startMs <= merged.back().endMs + kMergeGapMs) {
            Segment& m = merged.back();
            m.endMs = std::max(m.endMs, s.endMs);
            if (m.videoDurationMs == 0) m.videoDurationMs = s.videoDurationMs;
        } else {
            merged.push_back(std::move(s));
        }
    }
    std::erase_if(merged, [](const Segment& s) { return s.endMs - s.startMs < kMinSegmentMs; });
    return merged;
}

std::string SponsorBlock::sha256Hex(std::string_view data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("BCryptOpenAlgorithmProvider(SHA256) failed");
    unsigned char digest[32]{};
    BCRYPT_HASH_HANDLE hash = nullptr;
    NTSTATUS st = BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0);
    if (st >= 0) {
        static char empty = 0;
        char* in = data.empty() ? &empty : const_cast<char*>(data.data());
        st = BCryptHashData(hash, reinterpret_cast<PUCHAR>(in), static_cast<ULONG>(data.size()), 0);
        if (st >= 0) st = BCryptFinishHash(hash, digest, static_cast<ULONG>(sizeof digest), 0);
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    if (st < 0) throw std::runtime_error("SHA-256 (CNG) failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (unsigned char b : digest) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out;
}

std::vector<SponsorBlock::Segment> SponsorBlock::fetchSegments(const std::string& videoId,
                                                               const std::vector<std::string>& categories,
                                                               bool hashPrefix) {
    if (videoId.empty() || categories.empty()) return {};
    const std::string query = "categories=" + http::urlEncode(nlohmann::json(categories).dump()) +
                              "&actionTypes=" + http::urlEncode(R"(["skip"])");
    const http::Headers headers{{"Accept", "application/json"}};

    if (hashPrefix) {
        // k-anonymity: only 4 hex chars of the hash leave the machine; ~0.002% of all videos share them.
        // No fallback to the direct lookup on errors: that would send the full video id (the Settings copy
        // promises it is never sent). A failed lookup is retried later by the caller instead.
        const auto r = http::get(std::string(kApi) + "/" + sha256Hex(videoId).substr(0, 4) + "?" + query, headers);
        if (r.statusCode == 404) return {};   // nothing in this bucket for these categories
        if (!r.isSuccessStatusCode()) throw std::runtime_error(std::format("SponsorBlock HTTP {}", r.statusCode));
        return keepCategories(parseResponse(r.body, videoId), categories);
    }
    const auto r = http::get(std::string(kApi) + "?videoID=" + http::urlEncode(videoId) + "&" + query, headers);
    if (r.statusCode == 404) return {};   // "Not Found" = the video has no segments
    if (!r.isSuccessStatusCode()) throw std::runtime_error(std::format("SponsorBlock HTTP {}", r.statusCode));
    return keepCategories(parseResponse(r.body, videoId), categories);
}

} // namespace st::app
