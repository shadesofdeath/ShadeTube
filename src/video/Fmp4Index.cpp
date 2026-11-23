#include "video/Fmp4Index.h"

#include <algorithm>

namespace st::video {

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
bool is(const uint8_t* type, const char* name) { return std::equal(type, type + 4, name); }

// Child boxes of [off, end) in `b`: calls fn(offset, size, type, headerSize) for each; stops at a malformed one.
template <class Fn>
void children(const std::vector<uint8_t>& b, size_t off, size_t end, Fn fn) {
    while (off + 8 <= end) {
        const uint32_t size = be32(b.data() + off);
        if (size < 8 || off + size > end) return;
        fn(off, size, b.data() + off + 4);
        off += size;
    }
}

// The first video/any track's edit: renames its edts box to "free" in `init` and returns the media time of the edit
// (first elst entry) in microseconds, using the track's mdhd timescale.
int64_t takeEditList(std::vector<uint8_t>& init) {
    int64_t editUs = 0;
    children(init, 0, init.size(), [&](size_t moov, uint32_t moovSize, const uint8_t* t) {
        if (!is(t, "moov")) return;
        children(init, moov + 8, moov + moovSize, [&](size_t trak, uint32_t trakSize, const uint8_t* tt) {
            if (!is(tt, "trak")) return;
            uint32_t timescale = 0;
            int64_t mediaTime = 0;
            children(init, trak + 8, trak + trakSize, [&](size_t box, uint32_t boxSize, const uint8_t* bt) {
                if (is(bt, "mdia")) {
                    children(init, box + 8, box + boxSize, [&](size_t m, uint32_t mSize, const uint8_t* mt) {
                        if (!is(mt, "mdhd") || mSize < 32) return;
                        const uint8_t* p = init.data() + m + 8;
                        timescale = p[0] == 1 ? (mSize >= 44 ? be32(p + 20) : 0) : be32(p + 12);
                    });
                } else if (is(bt, "edts")) {
                    children(init, box + 8, box + boxSize, [&](size_t e, uint32_t eSize, const uint8_t* et) {
                        if (!is(et, "elst") || eSize < 16) return;
                        const uint8_t* p = init.data() + e + 8;
                        if (be32(p + 4) == 0) return;   // no entries
                        if (p[0] == 1 && eSize >= 16 + 20) mediaTime = static_cast<int64_t>(be64(p + 8 + 8));
                        else if (p[0] == 0 && eSize >= 16 + 12) mediaTime = static_cast<int32_t>(be32(p + 8 + 4));
                    });
                    std::copy_n("free", 4, init.begin() + static_cast<ptrdiff_t>(box + 4));
                }
            });
            if (timescale > 0 && mediaTime > 0 && editUs == 0) editUs = mediaTime * 1'000'000 / timescale;
        });
    });
    return editUs;
}

} // namespace

size_t Fmp4Index::segmentAt(int64_t us) const {
    auto it = std::upper_bound(segments.begin(), segments.end(), us, [](int64_t t, const Fmp4Segment& s) { return t < s.startUs; });
    return it == segments.begin() ? 0 : static_cast<size_t>(it - segments.begin() - 1);
}

Fmp4Index parseFmp4Index(std::span<const uint8_t> head, int64_t fileLength) {
    Fmp4Index index;
    const size_t n = head.size();
    size_t pos = 0;
    size_t initEnd = 0;
    bool sawMoov = false;
    while (pos + 8 <= n) {
        uint64_t size = be32(head.data() + pos);
        const uint8_t* type = head.data() + pos + 4;
        size_t header = 8;
        if (size == 1) {
            if (pos + 16 > n) break;
            size = be64(head.data() + pos + 8);
            header = 16;
        } else if (size == 0) {
            size = static_cast<uint64_t>(fileLength) - pos;
        }
        if (size < header) return {};
        if (is(type, "moov")) sawMoov = true;
        if (is(type, "moof")) break;   // no sidx before the media
        if (!is(type, "sidx")) {
            if (pos + size > n) return {};   // the init boxes must be in `head`
            initEnd = pos + static_cast<size_t>(size);
            pos += static_cast<size_t>(size);
            continue;
        }
        // sidx (ISO/IEC 14496-12 8.16.3)
        if (pos + size > n || !sawMoov) return {};
        const uint8_t* p = head.data() + pos + header;
        const uint8_t* end = head.data() + pos + size;
        if (end - p < 4 + 4 + 4) return {};
        const uint8_t version = p[0];
        p += 4;                         // version + flags
        p += 4;                         // reference_ID
        const uint32_t timescale = be32(p);
        p += 4;
        if (timescale == 0) return {};
        uint64_t earliest = 0, firstOffset = 0;
        if (version == 0) {
            if (end - p < 8) return {};
            earliest = be32(p);
            firstOffset = be32(p + 4);
            p += 8;
        } else {
            if (end - p < 16) return {};
            earliest = be64(p);
            firstOffset = be64(p + 8);
            p += 16;
        }
        if (end - p < 4) return {};
        const uint16_t count = static_cast<uint16_t>(p[2] << 8 | p[3]);
        p += 4;                         // reserved + reference_count
        if (end - p < static_cast<ptrdiff_t>(count) * 12) return {};
        int64_t offset = static_cast<int64_t>(pos + size + firstOffset);
        uint64_t t = earliest;
        for (uint16_t i = 0; i < count; ++i, p += 12) {
            const uint32_t ref = be32(p);
            if (ref & 0x80000000u) return {};   // a hierarchical index (points at another sidx): not used here
            Fmp4Segment s;
            s.offset = offset;
            s.size = ref & 0x7FFFFFFFu;
            s.startUs = static_cast<int64_t>(t * 1'000'000 / timescale);
            const uint32_t dur = be32(p + 4);
            s.durationUs = static_cast<int64_t>(uint64_t(dur) * 1'000'000 / timescale);
            if (s.offset + s.size > fileLength) return {};
            index.segments.push_back(s);
            offset += s.size;
            t += dur;
        }
        index.init.assign(head.begin(), head.begin() + static_cast<ptrdiff_t>(initEnd));
        index.editUs = takeEditList(index.init);
        return index;
    }
    return {};
}

} // namespace st::video
