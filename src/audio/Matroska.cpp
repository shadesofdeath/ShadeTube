#include "audio/Matroska.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace st::audio {

ByteReader::Status MemoryByteReader::read(int64_t offset, void* dst, size_t n) {
    if (offset < 0 || static_cast<uint64_t>(offset) > size_ || n > size_ - static_cast<size_t>(offset)) return Status::End;
    if (n) std::memcpy(dst, data_ + offset, n);
    return Status::Ok;
}

namespace mkv {

namespace {

constexpr uint32_t kEbml = 0x1A45DFA3, kDocType = 0x4282;
constexpr uint32_t kSegment = 0x18538067;
constexpr uint32_t kSeekHead = 0x114D9B74, kSeek = 0x4DBB, kSeekId = 0x53AB, kSeekPosition = 0x53AC;
constexpr uint32_t kInfo = 0x1549A966, kTimecodeScale = 0x2AD7B1, kDuration = 0x4489;
constexpr uint32_t kTracks = 0x1654AE6B, kTrackEntry = 0xAE, kTrackNumber = 0xD7, kTrackType = 0x83, kCodecId = 0x86,
                   kCodecPrivate = 0x63A2, kCodecDelay = 0x56AA, kSeekPreRoll = 0x56BB, kAudio = 0xE1,
                   kSamplingFrequency = 0xB5, kChannels = 0x9F;
constexpr uint32_t kCues = 0x1C53BB6B, kCuePoint = 0xBB, kCueTime = 0xB3, kCueTrackPositions = 0xB7, kCueTrack = 0xF7,
                   kCueClusterPosition = 0xF1;
constexpr uint32_t kCluster = 0x1F43B675, kTimecode = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1,
                   kDiscardPadding = 0x75A2;
constexpr uint32_t kTags = 0x1254C367, kChapters = 0x1043A770, kAttachments = 0x1941A469;

constexpr int64_t kMaxBlock = 16ll << 20;        // a block (frame data) larger than this is not audio
constexpr int64_t kMaxPrivate = 1ll << 20;       // CodecPrivate
constexpr size_t kMaxCues = 1'000'000;
constexpr int kMaxTopLevel = 4096;               // segment children looked at before the first cluster

bool topLevel(uint32_t id) {
    return id == kCluster || id == kCues || id == kTags || id == kChapters || id == kAttachments || id == kSeekHead ||
           id == kInfo || id == kTracks || id == kSegment || id == kEbml;
}

Result fromStatus(ByteReader::Status s) {
    switch (s) {
    case ByteReader::Status::Ok: return Result::Ok;
    case ByteReader::Status::End: return Result::End;
    case ByteReader::Status::Interrupted: return Result::Interrupted;
    case ByteReader::Status::Failed: return Result::Failed;
    }
    return Result::Failed;
}

// A failed / interrupted read stops everything; End inside a structure that should be there means truncated data.
bool fatal(Result r) { return r == Result::Interrupted || r == Result::Failed; }

} // namespace

Result Demuxer::header(int64_t pos, int64_t limit, Element& e) {
    if (pos < 0 || pos >= std::min(limit, fileSize_)) return Result::End;
    uint8_t b[12];
    const size_t avail = static_cast<size_t>(std::min<int64_t>(12, fileSize_ - pos));
    if (avail < 2) return Result::End;
    if (const Result r = fromStatus(r_->read(pos, b, avail)); r != Result::Ok) return r;
    if (b[0] == 0) return Result::Invalid;
    const size_t idLen = static_cast<size_t>(std::countl_zero(b[0])) + 1;
    if (idLen > 4) return Result::Invalid;
    if (idLen >= avail) return Result::End;
    uint32_t id = 0;
    for (size_t i = 0; i < idLen; ++i) id = id << 8 | b[i];
    const uint8_t s0 = b[idLen];
    if (s0 == 0) return Result::Invalid;
    const size_t sizeLen = static_cast<size_t>(std::countl_zero(s0)) + 1;
    if (idLen + sizeLen > avail) return Result::End;
    uint64_t value = s0 & (0xFFu >> sizeLen);
    bool allOnes = value == (0xFFu >> sizeLen);
    for (size_t i = 1; i < sizeLen; ++i) {
        value = value << 8 | b[idLen + i];
        allOnes = allOnes && b[idLen + i] == 0xFF;
    }
    e.id = id;
    e.start = pos;
    e.content = pos + static_cast<int64_t>(idLen + sizeLen);
    if (allOnes) e.size = -1;
    else if (value > static_cast<uint64_t>(INT64_MAX / 2)) return Result::Invalid;
    else e.size = static_cast<int64_t>(value);
    return Result::Ok;
}

Result Demuxer::readBytes(int64_t pos, size_t n, std::vector<uint8_t>& out) {
    out.resize(n);
    if (n == 0) return Result::Ok;
    return fromStatus(r_->read(pos, out.data(), n));
}

Result Demuxer::readUint(const Element& e, uint64_t& v) {
    if (e.size < 0 || e.size > 8) return Result::Invalid;
    uint8_t b[8];
    if (e.size > 0)
        if (const Result r = fromStatus(r_->read(e.content, b, static_cast<size_t>(e.size))); r != Result::Ok) return r;
    v = 0;
    for (int64_t i = 0; i < e.size; ++i) v = v << 8 | b[i];
    return Result::Ok;
}

Result Demuxer::readFloat(const Element& e, double& v) {
    uint64_t bits = 0;
    if (e.size != 0 && e.size != 4 && e.size != 8) return Result::Invalid;
    if (const Result r = readUint(e, bits); r != Result::Ok) return r;
    if (e.size == 4) v = std::bit_cast<float>(static_cast<uint32_t>(bits));
    else if (e.size == 8) v = std::bit_cast<double>(bits);
    else v = 0;
    return Result::Ok;
}

Result Demuxer::parseInfo(const Element& info) {
    double duration = 0;
    const int64_t end = info.end(segEnd_);
    Element e;
    for (int64_t p = info.content; p < end;) {
        Result r = header(p, end, e);
        if (r == Result::End) break;
        if (r != Result::Ok) return r;
        if (e.size < 0) return Result::Invalid;
        if (e.id == kTimecodeScale) {
            uint64_t v = 0;
            if (r = readUint(e, v); r != Result::Ok) return r;
            if (v > 0 && v <= 1'000'000'000ull) scale_ = v;
        } else if (e.id == kDuration) {
            if (r = readFloat(e, duration); r != Result::Ok) return r;
        }
        p = e.end(end);
    }
    if (duration > 0 && duration < 1e15) durationNs_ = static_cast<int64_t>(duration * static_cast<double>(scale_));
    return Result::Ok;
}

Result Demuxer::parseTracks(const Element& tracks) {
    const int64_t end = tracks.end(segEnd_);
    Element entry;
    for (int64_t p = tracks.content; p < end && track_.number == 0;) {
        Result r = header(p, end, entry);
        if (r == Result::End) break;
        if (r != Result::Ok) return r;
        if (entry.size < 0) return Result::Invalid;
        p = entry.end(end);
        if (entry.id != kTrackEntry) continue;
        AudioTrack t;
        uint64_t type = 0;
        const int64_t eEnd = entry.end(end);
        Element e;
        for (int64_t q = entry.content; q < eEnd;) {
            r = header(q, eEnd, e);
            if (r == Result::End) break;
            if (r != Result::Ok) return r;
            if (e.size < 0) return Result::Invalid;
            uint64_t v = 0;
            std::vector<uint8_t> bytes;
            switch (e.id) {
            case kTrackNumber:
                if (r = readUint(e, v); r != Result::Ok) return r;
                t.number = v;
                break;
            case kTrackType:
                if (r = readUint(e, v); r != Result::Ok) return r;
                type = v;
                break;
            case kCodecId:
                if (e.size > 64) break;
                if (r = readBytes(e.content, static_cast<size_t>(e.size), bytes); r != Result::Ok) return r;
                t.codecId.assign(bytes.begin(), std::find(bytes.begin(), bytes.end(), uint8_t(0)));
                break;
            case kCodecPrivate:
                if (e.size > kMaxPrivate) return Result::Invalid;
                if (r = readBytes(e.content, static_cast<size_t>(e.size), t.codecPrivate); r != Result::Ok) return r;
                break;
            case kCodecDelay:
                if (r = readUint(e, v); r != Result::Ok) return r;
                t.codecDelayNs = static_cast<int64_t>(std::min<uint64_t>(v, 10'000'000'000ull));
                break;
            case kSeekPreRoll:
                if (r = readUint(e, v); r != Result::Ok) return r;
                t.seekPreRollNs = static_cast<int64_t>(std::min<uint64_t>(v, 10'000'000'000ull));
                break;
            case kAudio: {
                const int64_t aEnd = e.end(eEnd);
                Element a;
                for (int64_t s = e.content; s < aEnd;) {
                    r = header(s, aEnd, a);
                    if (r == Result::End) break;
                    if (r != Result::Ok) return r;
                    if (a.size < 0) return Result::Invalid;
                    if (a.id == kSamplingFrequency) {
                        double f = 0;
                        if (r = readFloat(a, f); r != Result::Ok) return r;
                        if (f > 0 && f < 1e7) t.sampleRate = static_cast<uint32_t>(f + 0.5);
                    } else if (a.id == kChannels) {
                        if (r = readUint(a, v); r != Result::Ok) return r;
                        t.channels = static_cast<uint32_t>(std::min<uint64_t>(v, 255));
                    }
                    s = a.end(aEnd);
                }
                break;
            }
            default: break;
            }
            q = e.end(eEnd);
        }
        if (type != 2 || t.number == 0) continue;   // not audio
        t.codec = t.codecId == "A_OPUS" ? Codec::Opus : t.codecId == "A_VORBIS" ? Codec::Vorbis : Codec::Other;
        track_ = std::move(t);
    }
    return Result::Ok;
}

Result Demuxer::parseSeekHead(const Element& head) {
    const int64_t end = head.end(segEnd_);
    Element seek;
    for (int64_t p = head.content; p < end;) {
        Result r = header(p, end, seek);
        if (r == Result::End) break;
        if (r != Result::Ok) return r;
        if (seek.size < 0) return Result::Invalid;
        p = seek.end(end);
        if (seek.id != kSeek) continue;
        uint64_t id = 0, position = 0;
        bool havePos = false;
        const int64_t sEnd = seek.end(end);
        Element e;
        for (int64_t q = seek.content; q < sEnd;) {
            r = header(q, sEnd, e);
            if (r == Result::End) break;
            if (r != Result::Ok) return r;
            if (e.size < 0) return Result::Invalid;
            if (e.id == kSeekId) {
                if (r = readUint(e, id); r != Result::Ok) return r;   // the element id, as its bytes
            } else if (e.id == kSeekPosition) {
                if (r = readUint(e, position); r != Result::Ok) return r;
                havePos = true;
            }
            q = e.end(sEnd);
        }
        if (havePos && id == kCues && position < static_cast<uint64_t>(fileSize_)) cuesPos_ = segData_ + static_cast<int64_t>(position);
    }
    return Result::Ok;
}

Result Demuxer::parseCues(int64_t pos) {
    Element cues;
    Result r = header(pos, segEnd_, cues);
    if (r != Result::Ok) return r == Result::End ? Result::Invalid : r;
    if (cues.id != kCues || cues.size < 0) return Result::Invalid;
    std::vector<std::pair<int64_t, int64_t>> list;
    const int64_t end = cues.end(segEnd_);
    Element point;
    for (int64_t p = cues.content; p < end && list.size() < kMaxCues;) {
        r = header(p, end, point);
        if (r == Result::End) break;
        if (r != Result::Ok) return r;
        if (point.size < 0) return Result::Invalid;
        p = point.end(end);
        if (point.id != kCuePoint) continue;
        uint64_t time = 0;
        bool haveTime = false;
        int64_t cluster = -1;
        const int64_t pEnd = point.end(end);
        Element e;
        for (int64_t q = point.content; q < pEnd;) {
            r = header(q, pEnd, e);
            if (r == Result::End) break;
            if (r != Result::Ok) return r;
            if (e.size < 0) return Result::Invalid;
            if (e.id == kCueTime) {
                if (r = readUint(e, time); r != Result::Ok) return r;
                haveTime = true;
            } else if (e.id == kCueTrackPositions) {
                uint64_t trackNo = 0, clusterPos = 0;
                bool haveCluster = false;
                const int64_t tEnd = e.end(pEnd);
                Element t;
                for (int64_t s = e.content; s < tEnd;) {
                    r = header(s, tEnd, t);
                    if (r == Result::End) break;
                    if (r != Result::Ok) return r;
                    if (t.size < 0) return Result::Invalid;
                    if (t.id == kCueTrack) {
                        if (r = readUint(t, trackNo); r != Result::Ok) return r;
                    } else if (t.id == kCueClusterPosition) {
                        if (r = readUint(t, clusterPos); r != Result::Ok) return r;
                        haveCluster = true;
                    }
                    s = t.end(tEnd);
                }
                if (haveCluster && (trackNo == 0 || trackNo == track_.number) && cluster < 0 &&
                    clusterPos < static_cast<uint64_t>(fileSize_))
                    cluster = segData_ + static_cast<int64_t>(clusterPos);
            }
            q = e.end(pEnd);
        }
        if (haveTime && cluster >= 0 && time < static_cast<uint64_t>(INT64_MAX) / scale_)
            list.emplace_back(static_cast<int64_t>(time * scale_), cluster);
    }
    std::sort(list.begin(), list.end());
    cues_ = std::move(list);
    return Result::Ok;
}

Result Demuxer::open(ByteReader& reader) {
    r_ = &reader;
    fileSize_ = reader.size();
    track_ = {};
    cues_.clear();
    pending_.clear();
    Element ebml;
    Result r = header(0, fileSize_, ebml);
    if (r != Result::Ok) return fatal(r) ? r : Result::Invalid;
    if (ebml.id != kEbml || ebml.size < 0 || ebml.size > 4096) return Result::Invalid;
    std::string docType;
    Element e;
    for (int64_t p = ebml.content; p < ebml.end(fileSize_);) {
        r = header(p, ebml.end(fileSize_), e);
        if (r == Result::End) break;
        if (r != Result::Ok) return fatal(r) ? r : Result::Invalid;
        if (e.size < 0) return Result::Invalid;
        if (e.id == kDocType && e.size <= 64) {
            std::vector<uint8_t> b;
            if (r = readBytes(e.content, static_cast<size_t>(e.size), b); r != Result::Ok) return fatal(r) ? r : Result::Invalid;
            docType.assign(b.begin(), std::find(b.begin(), b.end(), uint8_t(0)));
        }
        p = e.end(fileSize_);
    }
    if (docType != "webm" && docType != "matroska") return Result::Invalid;

    // The Segment (a Void element may come first).
    Element seg;
    int64_t p = ebml.end(fileSize_);
    for (int i = 0;; ++i) {
        r = header(p, fileSize_, seg);
        if (r != Result::Ok) return fatal(r) ? r : Result::Invalid;
        if (seg.id == kSegment) break;
        if (seg.size < 0 || i > 16) return Result::Invalid;
        p = seg.end(fileSize_);
    }
    segData_ = seg.content;
    segEnd_ = seg.size < 0 ? fileSize_ : seg.content + seg.size;

    // Top-level elements up to the first cluster.
    bool haveTracks = false;
    p = segData_;
    for (int i = 0; i < kMaxTopLevel; ++i) {
        r = header(p, segEnd_, e);
        if (r == Result::End) break;
        if (r != Result::Ok) return fatal(r) ? r : Result::Invalid;
        if (e.id == kCluster) {
            firstCluster_ = e.start;
            break;
        }
        if (e.size < 0) return Result::Invalid;
        Result sub = Result::Ok;
        switch (e.id) {
        case kSeekHead: sub = parseSeekHead(e); break;
        case kInfo: sub = parseInfo(e); break;
        case kTracks:
            sub = parseTracks(e);
            haveTracks = true;
            break;
        case kCues:
            cuesPos_ = e.start;
            sub = parseCues(e.start);
            cuesRead_ = true;
            break;
        default: break;
        }
        if (fatal(sub)) return sub;
        if (sub != Result::Ok && e.id != kCues) return Result::Invalid;
        p = e.end(segEnd_);
    }
    if (!haveTracks || track_.number == 0) return Result::Invalid;
    pos_ = firstCluster_ >= 0 ? firstCluster_ : segEnd_;
    inCluster_ = false;
    return Result::Ok;
}

Result Demuxer::parseBlock(const Element& e, bool simple, int64_t discardPaddingNs) {
    if (e.size < 0 || e.size > kMaxBlock) return Result::Invalid;
    if (e.size < 4) return Result::Ok;   // nothing in it
    std::vector<uint8_t> b;
    if (const Result r = readBytes(e.content, static_cast<size_t>(e.size), b); r != Result::Ok) return r;
    const size_t n = b.size();
    if (b[0] == 0) return Result::Ok;
    const size_t tl = static_cast<size_t>(std::countl_zero(b[0])) + 1;
    if (tl > 8 || tl + 3 > n) return Result::Ok;
    uint64_t trackNo = b[0] & (0xFFu >> tl);
    for (size_t i = 1; i < tl; ++i) trackNo = trackNo << 8 | b[i];
    if (trackNo != track_.number) return Result::Ok;
    const int16_t rel = static_cast<int16_t>(uint16_t(b[tl]) << 8 | b[tl + 1]);
    const uint8_t flags = b[tl + 2];
    (void)simple;
    const int64_t timeNs = (clusterTime_ + rel) * static_cast<int64_t>(scale_);
    size_t p = tl + 3;
    const int lacing = (flags >> 1) & 3;
    std::vector<size_t> sizes;
    if (lacing == 0) {
        sizes.push_back(n - p);
    } else {
        if (p >= n) return Result::Ok;
        const size_t count = size_t(b[p++]) + 1;
        size_t total = 0;
        if (lacing == 1) {   // Xiph
            for (size_t i = 0; i + 1 < count; ++i) {
                size_t s = 0;
                for (;;) {
                    if (p >= n) return Result::Ok;
                    const uint8_t v = b[p++];
                    s += v;
                    if (v != 255) break;
                }
                sizes.push_back(s);
                total += s;
            }
        } else if (lacing == 3) {   // EBML: the first size, then signed differences
            int64_t prev = 0;
            for (size_t i = 0; i + 1 < count; ++i) {
                if (p >= n || b[p] == 0) return Result::Ok;
                const size_t len = static_cast<size_t>(std::countl_zero(b[p])) + 1;
                if (len > 8 || p + len > n) return Result::Ok;
                uint64_t v = b[p] & (0xFFu >> len);
                for (size_t k = 1; k < len; ++k) v = v << 8 | b[p + k];
                p += len;
                int64_t s;
                if (i == 0) s = static_cast<int64_t>(v);
                else s = prev + static_cast<int64_t>(v) - ((int64_t(1) << (7 * len - 1)) - 1);
                if (s < 0 || s > static_cast<int64_t>(n)) return Result::Ok;
                sizes.push_back(static_cast<size_t>(s));
                total += static_cast<size_t>(s);
                prev = s;
            }
        } else {   // fixed size
            if ((n - p) % count != 0) return Result::Ok;
            for (size_t i = 0; i + 1 < count; ++i) {
                sizes.push_back((n - p) / count);
                total += (n - p) / count;
            }
        }
        if (total > n - p) return Result::Ok;
        sizes.push_back(n - p - total);
    }
    for (size_t i = 0; i < sizes.size(); ++i) {
        Frame f;
        f.data.assign(b.begin() + static_cast<ptrdiff_t>(p), b.begin() + static_cast<ptrdiff_t>(p + sizes[i]));
        p += sizes[i];
        f.timeNs = i == 0 ? timeNs : Frame::kContinued;
        if (i + 1 == sizes.size()) f.discardPaddingNs = discardPaddingNs;
        pending_.push_back(std::move(f));
    }
    return Result::Ok;
}

Result Demuxer::next(Frame& out) {
    for (int guard = 0; guard < 1'000'000; ++guard) {
        if (!pending_.empty()) {
            out = std::move(pending_.front());
            pending_.pop_front();
            return Result::Ok;
        }
        Element e;
        if (!inCluster_) {
            Result r = header(pos_, segEnd_, e);
            if (r != Result::Ok) return r == Result::Invalid ? Result::End : r;   // junk after the last cluster: the end
            if (e.id == kCluster) {
                inCluster_ = true;
                clusterEnd_ = std::min(e.end(segEnd_), segEnd_);
                clusterTime_ = 0;
                pos_ = e.content;
                continue;
            }
            if (e.size < 0) return Result::End;
            pos_ = e.end(segEnd_);
            continue;
        }
        if (pos_ >= clusterEnd_) {
            inCluster_ = false;
            continue;
        }
        Result r = header(pos_, clusterEnd_, e);
        if (r == Result::End) {
            inCluster_ = false;
            pos_ = clusterEnd_;
            continue;
        }
        if (r != Result::Ok) return r == Result::Invalid ? Result::End : r;
        if (topLevel(e.id)) {   // an unknown-size cluster ended here
            inCluster_ = false;
            continue;
        }
        if (e.size < 0) return Result::End;
        const int64_t end = e.end(clusterEnd_);
        if (e.id == kTimecode) {
            uint64_t v = 0;
            if (r = readUint(e, v); r != Result::Ok) return r == Result::Invalid ? Result::End : r;
            clusterTime_ = static_cast<int64_t>(std::min<uint64_t>(v, uint64_t(INT64_MAX) / scale_ / 2));
        } else if (e.id == kSimpleBlock) {
            if (r = parseBlock(e, true, 0); fatal(r)) return r;
        } else if (e.id == kBlockGroup) {
            Element block{};
            bool haveBlock = false;
            int64_t discard = 0;
            Element c;
            for (int64_t q = e.content; q < end;) {
                r = header(q, end, c);
                if (r == Result::End) break;
                if (r != Result::Ok) return r == Result::Invalid ? Result::End : r;
                if (c.size < 0) break;
                if (c.id == kBlock) {
                    block = c;
                    haveBlock = true;
                } else if (c.id == kDiscardPadding) {
                    uint64_t v = 0;
                    if (r = readUint(c, v); fatal(r)) return r;
                    if (r == Result::Ok && c.size > 0) {   // signed
                        const int shift = 64 - 8 * static_cast<int>(c.size);
                        discard = static_cast<int64_t>(v << shift) >> shift;
                    }
                }
                q = c.end(end);
            }
            if (haveBlock)
                if (r = parseBlock(block, false, std::max<int64_t>(0, discard)); fatal(r)) return r;
        }
        pos_ = end;
    }
    return Result::Invalid;
}

Result Demuxer::clusterTimeAt(int64_t pos, Element& cluster, int64_t& timeNs) {
    Result r = header(pos, segEnd_, cluster);
    if (r != Result::Ok) return r;
    if (cluster.id != kCluster) return Result::Invalid;
    const int64_t end = std::min(cluster.end(segEnd_), segEnd_);
    Element e;
    int64_t p = cluster.content;
    for (int i = 0; i < 8 && p < end; ++i) {
        r = header(p, end, e);
        if (r != Result::Ok) return r;
        if (e.size < 0) return Result::Invalid;
        if (e.id == kTimecode) {
            uint64_t v = 0;
            if (r = readUint(e, v); r != Result::Ok) return r;
            timeNs = static_cast<int64_t>(std::min<uint64_t>(v, uint64_t(INT64_MAX) / scale_ / 2) * scale_);
            return Result::Ok;
        }
        p = e.end(end);
    }
    return Result::Invalid;
}

Result Demuxer::seek(int64_t ns) {
    pending_.clear();
    inCluster_ = false;
    if (firstCluster_ < 0) {
        pos_ = segEnd_;
        return Result::Ok;
    }
    if (!cuesRead_) {
        if (cuesPos_ >= 0) {
            const Result r = parseCues(cuesPos_);
            if (fatal(r)) return r;   // read again at the next seek
        }
        cuesRead_ = true;
    }
    int64_t target = firstCluster_;
    if (!cues_.empty()) {
        auto it = std::upper_bound(cues_.begin(), cues_.end(), std::pair<int64_t, int64_t>{ns, INT64_MAX});
        if (it != cues_.begin()) target = std::prev(it)->second;
    } else if (ns > 0) {   // no index: walk the cluster headers
        int64_t p = firstCluster_;
        for (int i = 0; i < 1'000'000; ++i) {
            Element cluster;
            int64_t t = 0;
            const Result r = clusterTimeAt(p, cluster, t);
            if (fatal(r)) return r;
            if (r != Result::Ok || t > ns) break;
            target = p;
            if (cluster.size < 0) break;
            p = cluster.end(segEnd_);
        }
    }
    pos_ = target;
    return Result::Ok;
}

} // namespace mkv
} // namespace st::audio
