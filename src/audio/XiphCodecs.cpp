#include "audio/XiphCodecs.h"

#include <opus.h>
#include <opus_multistream.h>
#include <vorbis/codec.h>

#include <algorithm>
#include <cstring>

namespace st::audio::xiph {

namespace {
constexpr int kMaxOpusFrames = 5760;   // 120 ms at 48 kHz, the longest Opus packet

uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
} // namespace

// ---- Opus -----------------------------------------------------------------------------------------------------------

std::optional<OpusHead> parseOpusHead(const uint8_t* p, size_t n) {
    if (!p || n < 19 || std::memcmp(p, "OpusHead", 8) != 0) return std::nullopt;
    if ((p[8] & 0xF0) != 0) return std::nullopt;   // a major version this decoder doesn't know
    OpusHead h;
    h.channels = p[9];
    h.preSkip = le16(p + 10);
    h.inputRate = le32(p + 12);
    h.outputGainQ8 = static_cast<int16_t>(le16(p + 16));
    h.family = p[18];
    if (h.channels == 0) return std::nullopt;
    if (h.family == 0) {
        if (h.channels > 2) return std::nullopt;
        h.streams = 1;
        h.coupled = h.channels - 1;
        h.mapping = h.channels == 1 ? std::vector<uint8_t>{0} : std::vector<uint8_t>{0, 1};
        return h;
    }
    if (n < 21 + size_t(h.channels)) return std::nullopt;
    h.streams = p[19];
    h.coupled = p[20];
    if (h.streams == 0 || h.coupled > h.streams || h.streams + h.coupled > 255) return std::nullopt;
    h.mapping.assign(p + 21, p + 21 + h.channels);
    for (const uint8_t m : h.mapping)
        if (m != 255 && m >= h.streams + h.coupled) return std::nullopt;
    return h;
}

int opusPacketSamples(const uint8_t* p, size_t n) {
    if (!p || n == 0 || n > 0x7FFFFFFF) return 0;
    const int s = opus_packet_get_nb_samples(p, static_cast<opus_int32>(n), 48000);
    return s > 0 ? s : 0;
}

OpusPacketDecoder::~OpusPacketDecoder() { close(); }

void OpusPacketDecoder::close() {
    if (dec_) opus_multistream_decoder_destroy(dec_);
    dec_ = nullptr;
    head_ = {};
}

bool OpusPacketDecoder::open(const uint8_t* head, size_t n) {
    close();
    const auto h = parseOpusHead(head, n);
    if (!h || h->family == 3) return false;   // family 3 needs the projection (ambisonics demixing) decoder
    int err = 0;
    dec_ = opus_multistream_decoder_create(48000, static_cast<int>(h->channels), static_cast<int>(h->streams),
                                           static_cast<int>(h->coupled), h->mapping.data(), &err);
    if (!dec_ || err != OPUS_OK) {
        if (dec_) opus_multistream_decoder_destroy(dec_);
        dec_ = nullptr;
        return false;
    }
    if (h->outputGainQ8 != 0) opus_multistream_decoder_ctl(dec_, OPUS_SET_GAIN(h->outputGainQ8));
    head_ = *h;
    scratch_.resize(size_t(kMaxOpusFrames) * h->channels);
    return true;
}

void OpusPacketDecoder::reset() {
    if (dec_) opus_multistream_decoder_ctl(dec_, OPUS_RESET_STATE);
}

int OpusPacketDecoder::decode(const uint8_t* p, size_t n, std::vector<float>& out) {
    if (!dec_) return -1;
    if (n > 0x7FFFFFFF) return -1;
    const int frames = opus_multistream_decode_float(dec_, p, static_cast<opus_int32>(n), scratch_.data(), kMaxOpusFrames, 0);
    if (frames < 0) return -1;
    out.insert(out.end(), scratch_.begin(), scratch_.begin() + static_cast<ptrdiff_t>(size_t(frames) * head_.channels));
    return frames;
}

// ---- Vorbis ---------------------------------------------------------------------------------------------------------

struct VorbisPacketDecoder::State {
    vorbis_info vi{};
    vorbis_comment vc{};
    vorbis_dsp_state vd{};
    vorbis_block vb{};
    bool headers = false;
    bool synthesis = false;
    int64_t packetNo = 3;
    State() {
        vorbis_info_init(&vi);
        vorbis_comment_init(&vc);
    }
    ~State() {
        if (synthesis) {
            vorbis_block_clear(&vb);
            vorbis_dsp_clear(&vd);
        }
        vorbis_comment_clear(&vc);
        vorbis_info_clear(&vi);
    }
};

VorbisPacketDecoder::VorbisPacketDecoder() = default;
VorbisPacketDecoder::~VorbisPacketDecoder() = default;

void VorbisPacketDecoder::close() { s_.reset(); }
bool VorbisPacketDecoder::isOpen() const { return s_ && s_->synthesis; }
uint32_t VorbisPacketDecoder::sampleRate() const { return s_ && s_->headers ? static_cast<uint32_t>(s_->vi.rate) : 0; }
uint32_t VorbisPacketDecoder::channels() const { return s_ && s_->headers ? static_cast<uint32_t>(s_->vi.channels) : 0; }

bool VorbisPacketDecoder::open(const std::vector<std::vector<uint8_t>>& headers, bool synthesis) {
    close();
    if (headers.size() != 3) return false;
    auto s = std::make_unique<State>();
    for (size_t i = 0; i < 3; ++i) {
        ogg_packet op{};
        op.packet = const_cast<unsigned char*>(headers[i].data());
        op.bytes = static_cast<long>(headers[i].size());
        op.b_o_s = i == 0;
        op.packetno = static_cast<ogg_int64_t>(i);
        op.granulepos = -1;
        if (headers[i].empty() || vorbis_synthesis_headerin(&s->vi, &s->vc, &op) != 0) return false;
    }
    if (s->vi.channels <= 0 || s->vi.rate <= 0) return false;
    s->headers = true;
    if (!synthesis) {
        s_ = std::move(s);
        return true;
    }
    if (vorbis_synthesis_init(&s->vd, &s->vi) != 0) return false;
    if (vorbis_block_init(&s->vd, &s->vb) != 0) {
        vorbis_dsp_clear(&s->vd);
        return false;
    }
    s->synthesis = true;
    s_ = std::move(s);
    return true;
}

bool VorbisPacketDecoder::openLaced(const uint8_t* p, size_t n) {
    const auto headers = splitXiphLacing(p, n);
    return headers && open(*headers);
}

void VorbisPacketDecoder::reset() {
    if (isOpen()) vorbis_synthesis_restart(&s_->vd);
}

int VorbisPacketDecoder::decode(const uint8_t* p, size_t n, std::vector<float>& out) {
    if (!isOpen() || n == 0 || n > 0x7FFFFFFF) return -1;
    ogg_packet op{};
    op.packet = const_cast<unsigned char*>(p);
    op.bytes = static_cast<long>(n);
    op.granulepos = -1;
    op.packetno = s_->packetNo++;
    const int r = vorbis_synthesis(&s_->vb, &op);
    if (r == OV_ENOTAUDIO) return 0;   // a stray header packet
    if (r != 0) return -1;
    if (vorbis_synthesis_blockin(&s_->vd, &s_->vb) != 0) return -1;
    const int ch = s_->vi.channels;
    int total = 0;
    float** pcm = nullptr;
    for (int k; (k = vorbis_synthesis_pcmout(&s_->vd, &pcm)) > 0;) {
        const size_t at = out.size();
        out.resize(at + size_t(k) * ch);
        float* d = out.data() + at;
        for (int i = 0; i < k; ++i)
            for (int c = 0; c < ch; ++c) *d++ = pcm[c][i];
        vorbis_synthesis_read(&s_->vd, k);
        total += k;
    }
    return total;
}

int VorbisPacketDecoder::blockSize(const uint8_t* p, size_t n) const {
    if (!s_ || !s_->headers || n == 0 || n > 0x7FFFFFFF) return 0;
    ogg_packet op{};
    op.packet = const_cast<unsigned char*>(p);
    op.bytes = static_cast<long>(n);
    const long bs = vorbis_packet_blocksize(&s_->vi, &op);
    return bs > 0 ? static_cast<int>(bs) : 0;
}

std::optional<std::vector<std::vector<uint8_t>>> splitXiphLacing(const uint8_t* p, size_t n) {
    if (!p || n < 1) return std::nullopt;
    const size_t count = size_t(p[0]) + 1;
    size_t pos = 1;
    std::vector<size_t> sizes;
    size_t total = 0;
    for (size_t i = 0; i + 1 < count; ++i) {
        size_t s = 0;
        for (;;) {
            if (pos >= n) return std::nullopt;
            const uint8_t v = p[pos++];
            s += v;
            if (v != 255) break;
        }
        sizes.push_back(s);
        total += s;
    }
    if (total > n - pos) return std::nullopt;
    sizes.push_back(n - pos - total);
    std::vector<std::vector<uint8_t>> packets;
    for (const size_t s : sizes) {
        packets.emplace_back(p + pos, p + pos + s);
        pos += s;
    }
    return packets;
}

// ---- channels / comments --------------------------------------------------------------------------------------------

void appendStereo(const float* in, size_t frames, uint32_t ch, std::vector<float>& out) {
    if (ch == 2) {
        out.insert(out.end(), in, in + frames * 2);
        return;
    }
    const size_t at = out.size();
    out.resize(at + frames * 2);
    float* d = out.data() + at;
    if (ch == 1) {
        for (size_t i = 0; i < frames; ++i) d[2 * i] = d[2 * i + 1] = in[i];
        return;
    }
    constexpr float c = 0.70710678f;
    // (left, right) weight of each input channel, Vorbis order.
    float wl[8] = {}, wr[8] = {};
    auto set = [&](int i, float l, float r) {
        wl[i] = l;
        wr[i] = r;
    };
    switch (ch) {
    case 3: set(0, 1, 0), set(1, c, c), set(2, 0, 1); break;
    case 4: set(0, 1, 0), set(1, 0, 1), set(2, c, 0), set(3, 0, c); break;
    case 5:
    case 6: set(0, 1, 0), set(1, c, c), set(2, 0, 1), set(3, c, 0), set(4, 0, c); break;   // 6: + LFE (dropped)
    case 7: set(0, 1, 0), set(1, c, c), set(2, 0, 1), set(3, c, 0), set(4, 0, c), set(5, 0.5f, 0.5f); break;
    case 8: set(0, 1, 0), set(1, c, c), set(2, 0, 1), set(3, c, 0), set(4, 0, c), set(5, c, 0), set(6, 0, c); break;
    default: set(0, 1, 0), set(1, 0, 1); break;   // > 8: the first two
    }
    float norm = 0;
    for (const float w : wl) norm += w;
    for (int i = 0; i < 8; ++i) {
        wl[i] /= norm;
        wr[i] /= norm;
    }
    const uint32_t used = std::min<uint32_t>(ch, 8);
    for (size_t f = 0; f < frames; ++f) {
        const float* s = in + f * ch;
        float l = 0, r = 0;
        for (uint32_t i = 0; i < used; ++i) {
            l += s[i] * wl[i];
            r += s[i] * wr[i];
        }
        d[2 * f] = l;
        d[2 * f + 1] = r;
    }
}

std::vector<std::pair<std::string, std::string>> parseComments(const uint8_t* p, size_t n) {
    std::vector<std::pair<std::string, std::string>> out;
    if (!p || n < 8) return out;
    size_t pos = 0;
    const uint32_t vendor = le32(p);
    pos += 4;
    if (vendor > n - pos) return out;
    pos += vendor;
    if (n - pos < 4) return out;
    uint32_t count = le32(p + pos);
    pos += 4;
    while (count-- > 0 && n - pos >= 4) {
        const uint32_t len = le32(p + pos);
        pos += 4;
        if (len > n - pos) break;
        const std::string_view entry(reinterpret_cast<const char*>(p + pos), len);
        pos += len;
        const size_t eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        std::string key(entry.substr(0, eq));
        for (char& ch : key)
            if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
        out.emplace_back(std::move(key), std::string(entry.substr(eq + 1)));
    }
    return out;
}

} // namespace st::audio::xiph
