#include "audio/LiveStream.h"

#include "audio/HttpStream.h"
#include "core/Log.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <format>
#include <map>

namespace st::audio {

namespace {

constexpr uint32_t kBackoffMs[] = {500, 1000, 2000, 4000, 8000};
constexpr int64_t kHealthyUs = 10'000'000;           // a session that delivered this much resets the failure count
constexpr size_t kMaxPlaylistBytes = 512 * 1024;
constexpr size_t kMaxInitBytes = 2 * 1024 * 1024;
constexpr size_t kMaxSegmentBytes = 16 * 1024 * 1024;
constexpr size_t kMaxHunt = 512 * 1024;              // bytes without a frame: not an MPEG / AAC stream (or lost)
constexpr int kMaxPlaylistDepth = 4;
constexpr size_t kReadChunk = 16 * 1024;
constexpr size_t kMaxKeys = 8;

std::string_view trimLeft(std::string_view s) {
    while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') s.remove_prefix(1);
    return s;
}

bool looksLikeFmp4(const std::string& d) {
    if (d.size() < 8) return false;
    const std::string_view type(d.data() + 4, 4);
    return type == "ftyp" || type == "styp" || type == "moof" || type == "sidx" || type == "emsg" || type == "prft";
}

// AES-128-CBC with PKCS#7 padding (HLS METHOD=AES-128), in place.
bool aes128Decrypt(std::string& data, const std::string& key, std::array<uint8_t, 16> iv) {
    if (key.size() != 16 || data.empty() || data.size() % 16 != 0) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    BCRYPT_KEY_HANDLE handle = nullptr;
    auto* mode = const_cast<PUCHAR>(reinterpret_cast<const UCHAR*>(BCRYPT_CHAIN_MODE_CBC));
    bool ok = BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, mode, sizeof(BCRYPT_CHAIN_MODE_CBC), 0) == 0 &&
              BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())), 16, 0) == 0;
    if (ok) {
        ULONG out = 0;
        auto* p = reinterpret_cast<PUCHAR>(data.data());
        ok = BCryptDecrypt(handle, p, static_cast<ULONG>(data.size()), nullptr, iv.data(), 16, p, static_cast<ULONG>(data.size()), &out,
                           BCRYPT_BLOCK_PADDING) == 0;
        if (ok) data.resize(out);
    }
    if (handle) BCryptDestroyKey(handle);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

} // namespace

LiveStream::LiveStream(std::string url, std::string mimeHint, std::atomic<int64_t>* memCounter, bool allowLocalNetwork)
    : url_(std::move(url)), mimeHint_(std::move(mimeHint)), memCounter_(memCounter), allowLocalNetwork_(allowLocalNetwork) {
    cancelEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    info_.streamUrl = url_;
}

LiveStream::~LiveStream() {
    cancel();
    join();
    if (cancelEvent_) CloseHandle(cancelEvent_);
}

void LiveStream::start() { thread_ = std::thread([this] { run(); }); }

void LiveStream::cancel() {
    cancelled_.store(true, std::memory_order_release);
    if (cancelEvent_) SetEvent(cancelEvent_);
    std::lock_guard lock(mutex_);
    dataCv_.notify_all();
    spaceCv_.notify_all();
}

void LiveStream::join() {
    if (thread_.joinable()) thread_.join();
    std::lock_guard lock(mutex_);
    if (memCounter_ && queuedBytes_) memCounter_->fetch_sub(static_cast<int64_t>(queuedBytes_));
    queue_.clear();
    queuedUs_ = 0;
    queuedBytes_ = 0;
}

// ---- consumer side ------------------------------------------------------------------------------------------------

LiveStream::Pop LiveStream::pop(live::Frame& out) {
    std::unique_lock lock(mutex_);
    dataCv_.wait(lock, [&] { return cancelled_.load() || !queue_.empty() || failed_; });
    if (cancelled_.load()) return Pop::Cancelled;
    if (queue_.empty()) return Pop::Failed;
    out = std::move(queue_.front());
    queue_.pop_front();
    queuedUs_ -= live::frameDurationUs(out);
    queuedBytes_ -= out.data.size();
    if (memCounter_) memCounter_->fetch_sub(static_cast<int64_t>(out.data.size()));
    spaceCv_.notify_one();
    return Pop::Frame;
}

int64_t LiveStream::bufferedMs() const {
    std::lock_guard lock(mutex_);
    return queuedUs_ / 1000;
}

void LiveStream::setPaused(bool paused) {
    std::lock_guard lock(mutex_);
    if (paused_ == paused) return;
    paused_ = paused;
    if (paused) pausedAt_ = GetTickCount64();
    else suspended_ = false;   // reconnects if the pause had closed the connection
    spaceCv_.notify_all();
}

bool LiveStream::takeStale() {
    std::lock_guard lock(mutex_);
    const bool stale = stale_;
    stale_ = false;
    return stale;
}

ErrorKind LiveStream::errorKind() const {
    std::lock_guard lock(mutex_);
    return errorKind_;
}

std::string LiveStream::errorMessage() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void LiveStream::setDecodedFormat(uint32_t sampleRate, uint32_t channels) {
    std::lock_guard lock(mutex_);
    info_.sampleRate = sampleRate;
    info_.channels = channels;
}

LiveInfo LiveStream::info() const {
    std::lock_guard lock(mutex_);
    LiveInfo i = info_;
    i.active = true;
    i.bufferedMs = queuedUs_ / 1000;
    if (codec_ == live::Codec::Aac && codedRate_ && info_.sampleRate >= 2 * codedRate_) i.codec = "HE-AAC";   // SBR doubles the rate
    return i;
}

// ---- producer side ------------------------------------------------------------------------------------------------

bool LiveStream::push(live::Frame&& f) {
    const int64_t duration = live::frameDurationUs(f);
    std::unique_lock lock(mutex_);
    if (discontinuity_) {
        f.discontinuity = true;
        discontinuity_ = false;
    }
    for (;;) {
        if (cancelled_.load()) return false;
        if (paused_) {
            // Paused: keep only the newest audio (a title of a dropped frame moves on to the next one). The reading
            // goes on at network speed, so bytes are bounded too (huge frames of a hostile stream).
            bool dropped = false;
            while (!queue_.empty() && (queuedUs_ + duration > kPausedKeepMs * 1000 || queuedBytes_ + f.data.size() > kMaxQueueBytes)) {
                live::Frame& old = queue_.front();
                if (old.title) {
                    if (queue_.size() > 1 && !queue_[1].title) queue_[1].title = std::move(old.title);
                    else if (queue_.size() == 1 && !f.title) f.title = std::move(old.title);
                }
                queuedUs_ -= live::frameDurationUs(old);
                queuedBytes_ -= old.data.size();
                if (memCounter_) memCounter_->fetch_sub(static_cast<int64_t>(old.data.size()));
                queue_.pop_front();
                dropped = true;
            }
            if (dropped) {
                stale_ = true;
                if (!queue_.empty()) queue_.front().discontinuity = true;
                else f.discontinuity = true;
            }
            break;
        }
        if (queuedUs_ < kMaxQueueMs * 1000 && queuedBytes_ < kMaxQueueBytes) break;
        spaceCv_.wait_for(lock, std::chrono::milliseconds(250));   // full: back-pressure (re-checks pause / cancel)
    }
    queuedUs_ += duration;
    queuedBytes_ += f.data.size();
    if (memCounter_) memCounter_->fetch_add(static_cast<int64_t>(f.data.size()));
    // The first audio of each connection: what plays (the log of a station that "does not play" starts here).
    if (deliveredUs_ == 0)
        ST_LOG_INFO("audio", "live stream audio: {} {} Hz / {} ch{}{}", live::codecName(f), f.sampleRate, f.channels,
                    info_.hls ? ", HLS" : "", info_.reconnects ? std::format(", after {} reconnect(s)", info_.reconnects) : "");
    deliveredUs_ += duration;
    // Stream info: codec, coded rate, bitrate (header, icy-br, else measured over the session).
    codec_ = f.codec;
    codedRate_ = f.sampleRate;
    info_.codec = live::codecName(f);
    if (!info_.sampleRate) info_.channels = f.channels;
    measuredBytes_ += static_cast<int64_t>(f.data.size());
    measuredUs_ += duration;
    if (f.bitrate) info_.bitrateKbps = f.bitrate / 1000;
    else if (headerKbps_) info_.bitrateKbps = headerKbps_;
    else if (measuredUs_ >= 2'000'000) info_.bitrateKbps = static_cast<uint32_t>(measuredBytes_ * 8'000 / measuredUs_);
    queue_.push_back(std::move(f));
    dataCv_.notify_one();
    return true;
}

bool LiveStream::suspendDue() {
    std::lock_guard lock(mutex_);
    return paused_ && GetTickCount64() - pausedAt_ >= static_cast<uint64_t>(kSuspendAfterMs);
}

void LiveStream::suspend() {
    std::lock_guard lock(mutex_);
    ST_LOG_INFO("audio", "live stream paused for {} s: disconnected", kSuspendAfterMs / 1000);
    suspended_ = paused_;   // resumed meanwhile: reconnect right away
    if (memCounter_ && queuedBytes_) memCounter_->fetch_sub(static_cast<int64_t>(queuedBytes_));
    queue_.clear();
    queuedUs_ = 0;
    queuedBytes_ = 0;
    stale_ = true;
    discontinuity_ = true;
}

void LiveStream::attachTitle(live::Frame& f) {
    if (!pendingTitle_) return;
    if (*pendingTitle_ != lastTitle_) {
        f.title = *pendingTitle_;
        lastTitle_ = *pendingTitle_;
    }
    pendingTitle_.reset();
}

void LiveStream::markDiscontinuity() {
    std::lock_guard lock(mutex_);
    discontinuity_ = true;
}

void LiveStream::fail(ErrorKind kind, std::string message) {
    ST_LOG_WARN("audio", "live stream failed: {}", message);
    std::lock_guard lock(mutex_);
    failed_ = true;
    errorKind_ = kind;
    error_ = std::move(message);
    info_.connected = false;
    dataCv_.notify_all();
}

bool LiveStream::sleep(uint32_t ms) { return WaitForSingleObject(cancelEvent_, ms) == WAIT_TIMEOUT; }

void LiveStream::run() {
    SetThreadDescription(GetCurrentThread(), L"st-live-stream");
    HttpStream http(cancelEvent_);
    http.setPublicOnly(!allowLocalNetwork_);
    int failures = 0;   // failed attempts in a row that count toward the limit (not paused)
    int retries = 0;    // failed attempts in a row, paused ones too: the backoff and the playlist entry to try
    bool everPlayed = false;
    while (!cancelled_.load()) {
        {   // closed after a long pause: wait for the resume
            std::unique_lock lock(mutex_);
            spaceCv_.wait(lock, [&] { return cancelled_.load() || !suspended_; });
        }
        if (cancelled_.load()) break;
        deliveredUs_ = 0;
        const Outcome o = session(http, retries);
        http.close();
        bool paused = false;
        {
            std::lock_guard lock(mutex_);
            info_.connected = false;
            paused = paused_;
        }
        if (cancelled_.load() || o.end == End::Cancelled) break;
        if (o.end == End::Fatal) {
            fail(o.kind, o.message);
            break;
        }
        if (deliveredUs_ > 0) everPlayed = true;
        if (deliveredUs_ >= kHealthyUs || (o.end == End::Ended && deliveredUs_ > 0)) failures = retries = 0;
        // Paused long enough (the session noticed it, or it dropped meanwhile): stay closed until the resume, which
        // starts afresh.
        if (o.end == End::Suspended || suspendDue()) {
            suspend();
            failures = retries = 0;
            continue;
        }
        ++retries;
        if (!paused) ++failures;
        const int limit = everPlayed ? kMaxReconnects : kMaxFirstAttempts;
        if (failures >= limit) {
            fail(ErrorKind::Network, o.message.empty() ? "stream unavailable" : o.message);
            break;
        }
        markDiscontinuity();
        {
            std::lock_guard lock(mutex_);
            ++info_.reconnects;
        }
        const uint32_t wait = kBackoffMs[std::min<size_t>(static_cast<size_t>(retries - 1), std::size(kBackoffMs) - 1)];
        ST_LOG_INFO("audio", "live stream {} ({}): reconnecting in {} ms ({}/{}{})", o.end == End::Ended ? "ended" : "dropped",
                    o.message, wait, failures, limit, paused ? ", paused" : "");
        if (paused) {   // the resume ends the wait: the listener is back, try now
            std::unique_lock lock(mutex_);
            spaceCv_.wait_for(lock, std::chrono::milliseconds(wait), [&] { return cancelled_.load() || !paused_; });
            if (cancelled_.load()) break;
        } else if (!sleep(wait)) {
            break;
        }
    }
    finished_.store(true, std::memory_order_release);
    std::lock_guard lock(mutex_);
    dataCv_.notify_all();
}

bool LiveStream::fetch(HttpStream& http, const std::string& url, int64_t offset, int64_t length, size_t maxBytes, std::string& body,
                       std::string& error) {
    HttpStream::Request r;
    r.url = url;
    r.rangeOffset = offset;
    r.rangeLength = length;
    body.clear();
    return http.open(r, error) && http.readAll(body, maxBytes, error);
}

LiveStream::Outcome LiveStream::session(HttpStream& http, int attempt) {
    std::string url = url_;
    if (!live::isHttpUrl(url)) return {End::Fatal, ErrorKind::Network, "not an http(s) stream URL"};   // e.g. an unknown station
    for (int depth = 0;; ++depth) {
        if (depth > kMaxPlaylistDepth) return {End::Fatal, ErrorKind::UnsupportedFormat, "too many nested playlists"};
        std::string error;
        HttpStream::Request request;
        request.url = url;
        request.icyMetadata = true;
        if (!http.open(request, error)) return {cancelled_.load() ? End::Cancelled : End::Dropped, ErrorKind::Network, error};
        // The first bytes decide what this is: radio servers often send wrong content types.
        std::string head;
        while (head.size() < 16) {
            char buf[4096];
            const int64_t n = http.read(buf, sizeof buf, error);
            if (n < 0) return {cancelled_.load() ? End::Cancelled : End::Dropped, ErrorKind::Network, error};
            if (n == 0) break;
            head.append(buf, static_cast<size_t>(n));
        }
        const std::string finalUrl = http.finalUrl();
        // The caller's MIME type stands in when the station's server sends none.
        const std::string contentType = http.contentType().empty() && depth == 0 ? mimeHint_ : http.contentType();
        const live::PlaylistType type = live::playlistType(contentType, head, finalUrl);
        if (type != live::PlaylistType::None) {
            std::string body = std::move(head);
            if (!http.readAll(body, kMaxPlaylistBytes, error)) return {End::Dropped, ErrorKind::Network, error};
            http.close();
            if (type == live::PlaylistType::Hls || body.find("#EXT-X-") != std::string::npos)
                return hlsSession(http, finalUrl, std::move(body));
            const auto urls = live::parsePlaylist(type, body, finalUrl);
            if (urls.empty()) return {End::Fatal, ErrorKind::UnsupportedFormat, "playlist without a stream URL"};
            url = urls[static_cast<size_t>(attempt) % urls.size()];   // after a failure, the next entry
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            info_.streamUrl = finalUrl;
            info_.stationName = live::decodeLegacyText(http.header("icy-name"));
            info_.hls = false;
            info_.connected = true;
            headerKbps_ = static_cast<uint32_t>(std::clamp(atoi(http.header("icy-br").c_str()), 0, 10000));
        }
        return pumpBody(http, std::move(head));
    }
}

LiveStream::Outcome LiveStream::pumpBody(HttpStream& http, std::string head) {
    const std::string_view start = trimLeft(head);
    if (start.starts_with("<")) return {End::Fatal, ErrorKind::UnsupportedFormat, "not an audio stream (HTML page)"};
    if (start.starts_with("fLaC")) return {End::Fatal, ErrorKind::UnsupportedFormat, "FLAC streams are not supported"};
    const size_t metaint = static_cast<size_t>(std::clamp(atoi(http.header("icy-metaint").c_str()), 0, 1 << 20));
    const bool finite = http.contentLength() > 0;
    // metaint counts from the first body byte, so the head is audio: Ogg shows right away.
    const bool ogg = head.starts_with("OggS");
    live::IcyDemuxer icy(metaint);
    live::FrameParser parser;
    live::OggDemuxer oggDemuxer;
    bool anyFrame = false, cancelled = false;

    auto drain = [&]() -> bool {
        live::Frame f;
        for (;;) {
            const bool got = ogg ? oggDemuxer.next(f) : parser.next(f);
            if (auto t = ogg ? oggDemuxer.takeTitle() : parser.takeTitle()) pendingTitle_ = std::move(*t);
            if (!got) return true;
            attachTitle(f);
            anyFrame = true;
            if (!push(std::move(f))) return false;
        }
    };
    // Frames are taken out after every piece of audio, so a title only goes to frames after its metadata block.
    auto onAudio = [&](const uint8_t* p, size_t n) {
        if (cancelled) return;
        if (ogg) oggDemuxer.push(p, n);
        else parser.push(p, n);
        cancelled = !drain();
    };
    auto onMeta = [&](std::string_view meta) {
        if (auto t = live::icyStreamTitle(meta)) pendingTitle_ = std::move(*t);
    };
    auto consume = [&](const uint8_t* p, size_t n) -> std::optional<Outcome> {
        icy.feed(p, n, onAudio, onMeta);
        if (cancelled) return Outcome{End::Cancelled};
        if (ogg && oggDemuxer.unsupported())
            return Outcome{End::Fatal, ErrorKind::UnsupportedFormat, std::format("Ogg {} streams are not supported", oggDemuxer.unsupportedCodec())};
        if ((ogg ? oggDemuxer.hunted() : parser.hunted()) > kMaxHunt) {
            if (anyFrame) return Outcome{End::Dropped, ErrorKind::Network, "lost the audio frames"};
            return Outcome{End::Fatal, ErrorKind::UnsupportedFormat, "no MP3 / AAC / Opus audio in the stream"};
        }
        if (suspendDue()) return Outcome{End::Suspended};
        return std::nullopt;
    };

    if (auto o = consume(reinterpret_cast<const uint8_t*>(head.data()), head.size())) return *o;
    std::vector<uint8_t> buf(kReadChunk);
    std::string error;
    for (;;) {
        const int64_t n = http.read(buf.data(), buf.size(), error);
        if (n < 0) return {cancelled_.load() ? End::Cancelled : End::Dropped, ErrorKind::Network, error};
        if (n == 0) return {finite ? End::Ended : End::Dropped, ErrorKind::Network, "the server closed the stream"};
        if (auto o = consume(buf.data(), static_cast<size_t>(n))) return *o;
    }
}

// `url`: the playlist's address after redirects (session()); `body`: its text. The media playlist is reloaded from the
// address it was requested by (a redirect may lead to an expiring, tokenized one), while its entries resolve against
// where it really came from (like browsers and hls.js: the response URL is the base).
LiveStream::Outcome LiveStream::hlsSession(HttpStream& http, std::string url, std::string body) {
    std::string error;
    live::HlsPlaylist pl = live::parseHls(body, url);
    std::string base = url;
    if (pl.master) {
        url = live::pickHlsMedia(pl);
        if (url.empty()) return {End::Fatal, ErrorKind::UnsupportedFormat, "no HLS variant with MP3 / AAC audio"};
        if (!fetch(http, url, -1, -1, kMaxPlaylistBytes, body, error))
            return {cancelled_.load() ? End::Cancelled : End::Dropped, ErrorKind::Network, error};
        base = http.finalUrl();
        pl = live::parseHls(body, base);
        if (pl.master) return {End::Fatal, ErrorKind::UnsupportedFormat, "nested HLS master playlists"};
    }
    {
        std::lock_guard lock(mutex_);
        info_.streamUrl = base;
        info_.hls = true;
        info_.connected = true;
        headerKbps_ = 0;
    }
    live::TsDemuxer ts;
    live::Fmp4Demuxer mp4;
    live::FrameParser parser;
    std::string currentMap;
    std::map<std::string, std::string> keys;
    std::vector<uint8_t> es;
    int64_t nextSeq = -1;
    uint64_t lastNew = GetTickCount64(), lastLoad = GetTickCount64(), lastAudio = GetTickCount64();
    int64_t audioSeen = 0;
    int playlistFailures = 0;
    // Live: start three segments from the end (the spec's minimum distance); VOD: from the start.
    auto startSeq = [&] { return pl.segments[pl.endList || pl.segments.size() <= 3 ? 0 : pl.segments.size() - 3].sequence; };
    auto restart = [&] {   // what follows does not continue what was parsed (lost audio, another encoder)
        markDiscontinuity();
        parser.reset();
        ts.resync();
    };

    for (;;) {
        if (!pl.segments.empty()) {
            if (nextSeq < 0) {
                nextSeq = startSeq();
            } else if (nextSeq < pl.segments.front().sequence) {
                nextSeq = pl.segments.front().sequence;   // fell behind the live window
                restart();
            } else if (nextSeq > pl.segments.back().sequence + 1 + std::max<int64_t>(3, static_cast<int64_t>(pl.segments.size()))) {
                // The media sequence went back by more than a window (an encoder restart numbers from 0 again; a
                // CDN's slightly stale copy is only a segment or two behind and is waited out).
                ST_LOG_INFO("audio", "HLS media sequence restarted ({} -> {})", nextSeq, pl.segments.back().sequence);
                nextSeq = startSeq();
                restart();
            }
        }
        bool progressed = false;
        for (const auto& s : pl.segments) {
            if (s.sequence < nextSeq) continue;
            if (cancelled_.load()) return {End::Cancelled};
            if (suspendDue()) return {End::Suspended};
            std::string key;
            if (!s.key.method.empty() && s.key.method != "NONE") {
                if (s.key.method != "AES-128")
                    return {End::Fatal, ErrorKind::UnsupportedFormat, std::format("encrypted HLS ({}) is not supported", s.key.method)};
                auto it = keys.find(s.key.uri);
                if (it == keys.end()) {
                    std::string k;
                    if (!fetch(http, s.key.uri, -1, -1, 64, k, error) || k.size() != 16)
                        return {cancelled_.load() ? End::Cancelled : End::Dropped, ErrorKind::Network, "HLS key: " + error};
                    if (keys.size() >= kMaxKeys) keys.clear();
                    it = keys.emplace(s.key.uri, std::move(k)).first;
                }
                key = it->second;
            }
            if (!s.mapUri.empty() && s.mapUri != currentMap) {
                std::string init;
                if (!fetch(http, s.mapUri, s.mapOffset, s.mapLength, kMaxInitBytes, init, error))
                    return {cancelled_.load() ? End::Cancelled : End::Dropped, ErrorKind::Network, "HLS init segment: " + error};
                if (!mp4.init(reinterpret_cast<const uint8_t*>(init.data()), init.size()))
                    return {End::Fatal, ErrorKind::UnsupportedFormat, "HLS fMP4 without MP3 / AAC audio"};
                currentMap = s.mapUri;
            }
            std::string data;
            bool ok = fetch(http, s.uri, s.rangeOffset, s.rangeLength, kMaxSegmentBytes, data, error);
            if (!ok && !cancelled_.load()) ok = fetch(http, s.uri, s.rangeOffset, s.rangeLength, kMaxSegmentBytes, data, error);
            if (ok && !key.empty()) {
                std::array<uint8_t, 16> iv = s.key.iv;
                if (!s.key.hasIv) {   // the media sequence number, big-endian
                    iv = {};
                    for (int i = 0; i < 8; ++i) iv[15 - i] = static_cast<uint8_t>(static_cast<uint64_t>(s.sequence) >> (8 * i));
                }
                ok = aes128Decrypt(data, key, iv);
                if (!ok) error = "decryption failed";
            }
            nextSeq = s.sequence + 1;
            if (!ok) {
                if (cancelled_.load()) return {End::Cancelled};
                ST_LOG_WARN("audio", "HLS segment {} skipped: {}", s.sequence, error);
                restart();
                continue;
            }
            if (s.discontinuity) restart();   // new TS program tables (an ad, another encoder) replace the old ones
            if (s.title) pendingTitle_ = *s.title;
            const auto* p = reinterpret_cast<const uint8_t*>(data.data());
            es.clear();
            if (data.size() >= 188 && p[0] == 0x47) {
                ts.feed(p, data.size(), es);
                ts.endOfSegment();
                if (auto t = ts.takeTitle()) pendingTitle_ = std::move(*t);
                if (ts.unsupported()) return {End::Fatal, ErrorKind::UnsupportedFormat, "HLS audio codec not supported (AC-3 / LATM / encrypted)"};
            } else if (mp4.ready() || looksLikeFmp4(data)) {
                if (!mp4.ready()) return {End::Fatal, ErrorKind::UnsupportedFormat, "HLS fMP4 segment without an init segment"};
                mp4.feed(p, data.size(), es);
            } else {
                es.assign(p, p + data.size());   // packed audio: ID3 + ADTS / MP3 frames
            }
            parser.push(es.data(), es.size());
            live::Frame f;
            for (;;) {
                const bool got = parser.next(f);
                if (auto t = parser.takeTitle()) pendingTitle_ = std::move(*t);
                if (!got) break;
                attachTitle(f);
                if (!push(std::move(f))) return {End::Cancelled};
            }
            if (parser.hunted() > kMaxHunt && deliveredUs_ == 0)
                return {End::Fatal, ErrorKind::UnsupportedFormat, "no MP3 / AAC audio in the HLS segments"};
            progressed = true;
            lastNew = GetTickCount64();
            if (deliveredUs_ != audioSeen) {
                audioSeen = deliveredUs_;
                lastAudio = lastNew;
            }
        }
        if (pl.endList && (pl.segments.empty() || nextSeq > pl.segments.back().sequence))
            return {End::Ended, ErrorKind::Network, "the HLS playlist ended"};
        // Reload after a target duration when the playlist had new segments, after half of one when it had none.
        const double target = pl.targetDuration > 0 ? std::min(pl.targetDuration, 30.0) : 6.0;
        const uint64_t interval = static_cast<uint64_t>(target * (progressed ? 1000 : 500));
        const uint64_t since = GetTickCount64() - lastLoad;
        if (since < interval && !sleep(static_cast<uint32_t>(interval - since))) return {End::Cancelled};
        const uint64_t patience = static_cast<uint64_t>(target * 3000) + 10'000;
        if (GetTickCount64() - lastNew > patience) return {End::Dropped, ErrorKind::Network, "the HLS playlist stopped updating"};
        // Segments keep coming but none of them has playable audio any more (e.g. a stream the demuxer lost).
        if (GetTickCount64() - lastAudio > patience) return {End::Dropped, ErrorKind::Network, "no audio in the new HLS segments"};
        if (suspendDue()) return {End::Suspended};
        lastLoad = GetTickCount64();
        if (!fetch(http, url, -1, -1, kMaxPlaylistBytes, body, error)) {
            if (cancelled_.load()) return {End::Cancelled};
            if (++playlistFailures >= 3) return {End::Dropped, ErrorKind::Network, "HLS playlist: " + error};
            continue;
        }
        playlistFailures = 0;
        pl = live::parseHls(body, http.finalUrl());
        if (pl.master) return {End::Fatal, ErrorKind::UnsupportedFormat, "the HLS media playlist turned into a master playlist"};
    }
}

} // namespace st::audio
