#include "audio/XiphDecoder.h"

#include "audio/Matroska.h"
#include "audio/ProgressiveBuffer.h"
#include "audio/ReplayGain.h"
#include "audio/Resampler.h"
#include "audio/XiphCodecs.h"
#include "core/Log.h"

#include <opusfile.h>
#include <vorbis/vorbisfile.h>

#include <mferror.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>

namespace st::audio {

namespace {

constexpr int kSeekMargin = 1024;          // source frames decoded before a seek target when resampling (kernel warm-up)
constexpr int64_t kDefaultPreRollNs = 80'000'000;   // Opus: decode this long before a seek target
constexpr int kMaxBadPackets = 64;         // corrupt packets in a row before a WebM stream counts as broken
constexpr int64_t kNoTime = INT64_MIN;

// The ProgressiveBuffer as random-access bytes, and as the sequential stream opusfile / vorbisfile read.
class BufferReader final : public ByteReader {
public:
    explicit BufferReader(std::shared_ptr<ProgressiveBuffer> b) : b_(std::move(b)), size_(b_->length()) {}
    int64_t size() const override { return size_; }
    Status read(int64_t offset, void* dst, size_t n) override {
        if (offset < 0 || offset > size_ || static_cast<int64_t>(n) > size_ - offset) return Status::End;
        if (n == 0) return Status::Ok;
        switch (b_->read(offset, dst, n)) {
        case ProgressiveBuffer::ReadStatus::Ok: return Status::Ok;
        case ProgressiveBuffer::ReadStatus::Interrupted:
        case ProgressiveBuffer::ReadStatus::Cancelled: interrupted = true; return Status::Interrupted;
        case ProgressiveBuffer::ReadStatus::Failed: failed = true; return Status::Failed;
        }
        return Status::Failed;
    }
    // Sequential reads at `pos`; false on an interrupted / failed read.
    bool readSome(void* dst, size_t n, size_t& got) {
        got = static_cast<size_t>(std::clamp<int64_t>(size_ - pos, 0, static_cast<int64_t>(n)));
        if (got == 0) return true;
        if (read(pos, dst, got) != Status::Ok) {
            got = 0;
            return false;
        }
        pos += static_cast<int64_t>(got);
        return true;
    }
    bool seekTo(int64_t offset, int whence) {
        const int64_t base = whence == SEEK_CUR ? pos : whence == SEEK_END ? size_ : 0;
        const int64_t p = base + offset;
        if (p < 0 || p > size_) return false;
        pos = p;
        return true;
    }
    void clearFlags() { interrupted = failed = false; }

    int64_t pos = 0;
    bool interrupted = false, failed = false;

private:
    std::shared_ptr<ProgressiveBuffer> b_;
    int64_t size_;
};

// ---- opusfile / vorbisfile callbacks
int opRead(void* s, unsigned char* ptr, int n) {
    size_t got = 0;
    if (n < 0 || !static_cast<BufferReader*>(s)->readSome(ptr, static_cast<size_t>(n), got)) return -1;
    return static_cast<int>(got);
}
int opSeek(void* s, opus_int64 offset, int whence) { return static_cast<BufferReader*>(s)->seekTo(offset, whence) ? 0 : -1; }
opus_int64 opTell(void* s) { return static_cast<BufferReader*>(s)->pos; }

size_t ovRead(void* ptr, size_t size, size_t nmemb, void* s) {
    size_t got = 0;
    if (size == 0) return 0;
    if (!static_cast<BufferReader*>(s)->readSome(ptr, size * nmemb, got)) {
        errno = EIO;   // vorbisfile tells an error from the end by errno
        return 0;
    }
    return got / size;
}
int ovSeek(void* s, ogg_int64_t offset, int whence) { return static_cast<BufferReader*>(s)->seekTo(offset, whence) ? 0 : -1; }
long ovTell(void* s) { return static_cast<long>(std::min<int64_t>(static_cast<BufferReader*>(s)->pos, LONG_MAX)); }

} // namespace

struct XiphDecoder::Impl {
    enum class Mode { OggOpus, OggVorbis, Mkv };
    explicit Impl(std::shared_ptr<ProgressiveBuffer> b) : reader(b), buffer(std::move(b)) {}
    ~Impl() {
        if (of) op_free(of);
        if (vfOpen) ov_clear(&vf);
    }

    BufferReader reader;
    std::shared_ptr<ProgressiveBuffer> buffer;
    Mode mode = Mode::OggOpus;
    OggOpusFile* of = nullptr;
    OggVorbis_File vf{};
    bool vfOpen = false;
    int vorbisRate = 0;                    // the current Ogg Vorbis link's rate
    mkv::Demuxer demux;
    mkv::Codec mkvCodec = mkv::Codec::None;
    xiph::OpusPacketDecoder opus;
    xiph::VorbisPacketDecoder vorbis;
    int64_t delayNs = 0;                   // Opus in WebM: CodecDelay (pre-skip)
    int64_t preRollNs = 0;
    int64_t nextSrc = kNoTime;             // source frame after the last decoded one (kNoTime: take the block time)
    int bad = 0;

    Resampler resampler;
    bool resync = true;                    // the next output starts a new run (open / seek)
    int64_t outBase = 0, produced = 0;
    bool ended = false;
    std::vector<float> raw, stereo;
};

XiphDecoder::XiphDecoder() = default;
XiphDecoder::~XiphDecoder() = default;

XiphDecoder::Kind XiphDecoder::probe(ProgressiveBuffer& buffer, HRESULT& hr) {
    hr = S_OK;
    uint8_t b[4] = {};
    if (buffer.length() < 4) return Kind::None;
    switch (buffer.read(0, b, 4)) {
    case ProgressiveBuffer::ReadStatus::Ok: break;
    case ProgressiveBuffer::ReadStatus::Failed: hr = MF_E_NET_READ; return Kind::None;
    default: hr = E_ABORT; return Kind::None;
    }
    if (std::memcmp(b, "OggS", 4) == 0) return Kind::Ogg;
    if (b[0] == 0x1A && b[1] == 0x45 && b[2] == 0xDF && b[3] == 0xA3) return Kind::Matroska;
    return Kind::None;
}

bool XiphDecoder::isOpen() const { return impl_ != nullptr; }

void XiphDecoder::close() {
    impl_.reset();
    srcRate_ = outRate_ = 0;
    durationHns_ = 0;
    replayGain_.reset();
}

const char* XiphDecoder::codecName() const {
    if (!impl_) return "";
    if (impl_->mode == Impl::Mode::OggOpus || (impl_->mode == Impl::Mode::Mkv && impl_->mkvCodec == mkv::Codec::Opus)) return "Opus";
    return "Vorbis";
}

HRESULT XiphDecoder::readFailure() const {
    if (impl_ && impl_->reader.failed) return MF_E_NET_READ;
    if (impl_ && impl_->reader.interrupted) return E_ABORT;
    return E_FAIL;
}

HRESULT XiphDecoder::open(const std::shared_ptr<ProgressiveBuffer>& buffer, Kind kind, const RatePolicy& rates) {
    close();
    if (!buffer || buffer->length() <= 0 || kind == Kind::None) return S_FALSE;
    impl_ = std::make_unique<Impl>(buffer);
    Impl& m = *impl_;
    if (kind == Kind::Ogg) {
        static const OpusFileCallbacks kOpus = {opRead, opSeek, opTell, nullptr};
        int err = 0;
        m.reader.pos = 0;
        m.of = op_open_callbacks(&m.reader, &kOpus, nullptr, 0, &err);
        if (m.of) {
            m.mode = Impl::Mode::OggOpus;
            srcRate_ = 48000;
            const ogg_int64_t total = op_pcm_total(m.of, -1);
            if (total > 0) durationHns_ = total * 10'000'000 / 48000;
            if (const OpusTags* tags = op_tags(m.of, -1)) {
                int q8 = 0;
                if (opus_tags_get_track_gain(tags, &q8) == 0) replayGain_ = static_cast<float>(q8) / 256.f + 5.f;
                else if (const char* rg = opus_tags_query(tags, "REPLAYGAIN_TRACK_GAIN", 0)) replayGain_ = replaygain::parseGain(rg);
            }
        } else {
            if (m.reader.interrupted || m.reader.failed) {
                const HRESULT hr = readFailure();
                close();
                return hr;
            }
            static const ov_callbacks kVorbis = {ovRead, ovSeek, nullptr, ovTell};
            m.reader.clearFlags();
            m.reader.pos = 0;
            if (ov_open_callbacks(&m.reader, &m.vf, nullptr, 0, kVorbis) != 0) {
                const bool io = m.reader.interrupted || m.reader.failed;
                const HRESULT hr = io ? readFailure() : S_FALSE;   // not Opus or Vorbis: Ogg FLAC, Speex...
                close();
                return hr;
            }
            m.vfOpen = true;
            m.mode = Impl::Mode::OggVorbis;
            const vorbis_info* vi = ov_info(&m.vf, 0);
            if (!vi || vi->rate <= 0 || vi->channels <= 0) {
                close();
                return E_FAIL;
            }
            srcRate_ = static_cast<uint32_t>(vi->rate);
            m.vorbisRate = vi->rate;
            const double total = ov_time_total(&m.vf, -1);
            if (total > 0) durationHns_ = static_cast<int64_t>(total * 1e7);
            if (vorbis_comment* vc = ov_comment(&m.vf, -1))
                if (const char* rg = vorbis_comment_query(vc, "REPLAYGAIN_TRACK_GAIN", 0)) replayGain_ = replaygain::parseGain(rg);
        }
    } else {
        const mkv::Result r = m.demux.open(m.reader);
        if (r != mkv::Result::Ok) {
            const HRESULT hr = r == mkv::Result::Interrupted ? E_ABORT : r == mkv::Result::Failed ? MF_E_NET_READ : S_FALSE;
            close();
            return hr;
        }
        const mkv::AudioTrack& t = m.demux.track();
        m.mode = Impl::Mode::Mkv;
        m.mkvCodec = t.codec;
        if (t.codec == mkv::Codec::Opus) {
            std::vector<uint8_t> head = t.codecPrivate;
            if (head.empty() && t.channels >= 1 && t.channels <= 2) {   // no CodecPrivate: a plain mono / stereo stream
                const uint16_t preSkip = static_cast<uint16_t>(t.codecDelayNs * 48000 / 1'000'000'000);
                head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, static_cast<uint8_t>(t.channels),
                        static_cast<uint8_t>(preSkip & 0xFF), static_cast<uint8_t>(preSkip >> 8), 0x80, 0xBB, 0, 0, 0, 0, 0};
            }
            if (!m.opus.open(head.data(), head.size())) {
                close();
                return S_FALSE;
            }
            srcRate_ = 48000;
            m.delayNs = t.codecDelayNs > 0 ? t.codecDelayNs : int64_t(m.opus.head().preSkip) * 1'000'000'000 / 48000;
            m.preRollNs = t.seekPreRollNs > 0 ? t.seekPreRollNs : kDefaultPreRollNs;
        } else if (t.codec == mkv::Codec::Vorbis) {
            if (!m.vorbis.openLaced(t.codecPrivate.data(), t.codecPrivate.size())) {
                close();
                return S_FALSE;
            }
            srcRate_ = m.vorbis.sampleRate();
            m.delayNs = t.codecDelayNs;   // ffmpeg writes the first packet's (discarded) samples as a CodecDelay
            m.preRollNs = 0;
        } else {
            close();
            return S_FALSE;   // AAC / FLAC / ... in Matroska: Media Foundation's job
        }
        durationHns_ = m.demux.durationNs() / 100;
    }
    outRate_ = rates.pick(srcRate_);
    m.resampler.configure(srcRate_, outRate_, 2);
    m.resync = true;
    ST_LOG_DEBUG("audio", "built-in {} decoder: {} Hz{}, {} ms", codecName(), srcRate_,
                 outRate_ != srcRate_ ? std::format(" -> {} Hz", outRate_) : std::string(), durationHns_ / 10'000);
    return S_OK;
}

HRESULT XiphDecoder::seek(int64_t hns) {
    if (!impl_) return MF_E_NOT_INITIALIZED;
    Impl& m = *impl_;
    m.reader.clearFlags();
    hns = std::max<int64_t>(0, hns);
    const int64_t margin = m.resampler.passthrough() ? 0 : kSeekMargin;
    const int64_t target = std::max<int64_t>(0, hns * srcRate_ / 10'000'000 - margin);
    switch (m.mode) {
    case Impl::Mode::OggOpus:
        if (op_pcm_seek(m.of, target) != 0) return readFailure();
        break;
    case Impl::Mode::OggVorbis:
        if (ov_pcm_seek(&m.vf, target) != 0) return readFailure();
        break;
    case Impl::Mode::Mkv: {
        const int64_t ns = std::max<int64_t>(0, target * 1'000'000'000 / srcRate_ - m.preRollNs);
        const mkv::Result r = m.demux.seek(ns);
        if (r == mkv::Result::Interrupted) return E_ABORT;
        if (r == mkv::Result::Failed) return MF_E_NET_READ;
        if (r != mkv::Result::Ok) return E_FAIL;
        m.opus.reset();
        m.vorbis.reset();
        m.nextSrc = kNoTime;
        m.bad = 0;
        break;
    }
    }
    m.resampler.reset();
    m.resync = true;
    m.ended = false;
    return S_OK;
}

// The next decoded run of source-rate stereo frames and the source position of its first frame (or the end).
bool XiphDecoder::readSource(std::vector<float>& stereo, int64_t& srcPos, bool& eos, HRESULT& hr) {
    Impl& m = *impl_;
    for (int guard = 0; guard < 256; ++guard) {
        m.reader.clearFlags();
        switch (m.mode) {
        case Impl::Mode::OggOpus: {
            const ogg_int64_t at = op_pcm_tell(m.of);
            m.raw.resize(5760 * 2);
            const int n = op_read_float_stereo(m.of, m.raw.data(), static_cast<int>(m.raw.size()));
            if (n == OP_HOLE) continue;   // a gap in the data (a damaged page): go on after it
            if (n < 0) {
                hr = readFailure();
                return false;
            }
            if (n == 0) {
                eos = true;
                return true;
            }
            srcPos = at;
            stereo.insert(stereo.end(), m.raw.begin(), m.raw.begin() + 2 * n);
            return true;
        }
        case Impl::Mode::OggVorbis: {
            const ogg_int64_t at = ov_pcm_tell(&m.vf);
            float** pcm = nullptr;
            int link = 0;
            const long n = ov_read_float(&m.vf, &pcm, 4096, &link);
            if (n == OV_HOLE) continue;
            if (n < 0) {
                hr = readFailure();
                return false;
            }
            if (n == 0) {
                eos = true;
                return true;
            }
            const vorbis_info* vi = ov_info(&m.vf, link);
            if (!vi || vi->channels <= 0) {
                hr = E_FAIL;
                return false;
            }
            if (vi->rate != m.vorbisRate) {   // a chained link at another rate: resample it to the output rate
                ST_LOG_INFO("audio", "Ogg Vorbis link at {} Hz (was {} Hz)", vi->rate, m.vorbisRate);
                m.vorbisRate = vi->rate;
                const int64_t keep = m.outBase + m.produced;
                m.resampler.configure(static_cast<uint32_t>(vi->rate), outRate_, 2);
                m.resampler.reset();
                m.outBase = keep;
                m.produced = 0;
            }
            const int ch = vi->channels;
            m.raw.resize(size_t(n) * ch);
            for (long i = 0; i < n; ++i)
                for (int c = 0; c < ch; ++c) m.raw[size_t(i) * ch + c] = pcm[c][i];
            xiph::appendStereo(m.raw.data(), static_cast<size_t>(n), static_cast<uint32_t>(ch), stereo);
            srcPos = at;
            return true;
        }
        case Impl::Mode::Mkv: {
            mkv::Frame f;
            const mkv::Result r = m.demux.next(f);
            if (r == mkv::Result::Interrupted || r == mkv::Result::Failed) {
                hr = r == mkv::Result::Interrupted ? E_ABORT : MF_E_NET_READ;
                return false;
            }
            if (r != mkv::Result::Ok) {   // the end (or a damaged tail)
                eos = true;
                return true;
            }
            m.raw.clear();
            int n = 0;
            uint32_t ch = 2;
            if (m.mkvCodec == mkv::Codec::Opus) {
                n = m.opus.decode(f.data.data(), f.data.size(), m.raw);
                ch = m.opus.channels();
            } else {
                n = m.vorbis.decode(f.data.data(), f.data.size(), m.raw);
                ch = m.vorbis.channels();
            }
            if (n < 0) {
                if (++m.bad > kMaxBadPackets) {
                    hr = E_FAIL;
                    return false;
                }
                continue;
            }
            m.bad = 0;
            int64_t start = m.nextSrc;
            if (f.timeNs != mkv::Frame::kContinued && (m.nextSrc == kNoTime || m.resync)) {
                // Block times are whole milliseconds (and muxers round them their own way): only the first frame of a
                // run takes its position from one. Opus streams of fixed-length packets start them on a grid
                // (k * duration - pre-skip); a block time within 1 ms of it is snapped onto it, which makes positions
                // after a seek exact.
                start = static_cast<int64_t>(std::llround(static_cast<double>(f.timeNs - m.delayNs) * srcRate_ / 1e9));
                if (m.mkvCodec == mkv::Codec::Opus && n > 0) {
                    const int64_t delay = static_cast<int64_t>(std::llround(static_cast<double>(m.delayNs) * srcRate_ / 1e9));
                    const int64_t off = ((start + delay) % n + n) % n;
                    const int64_t snapped = off <= n / 2 ? start - off : start + (n - off);
                    if (std::llabs(snapped - start) <= int64_t(srcRate_) / 1000) start = snapped;
                }
            } else if (start == kNoTime) {
                start = 0;
            }
            m.nextSrc = start + n;
            int64_t first = 0, last = n;
            if (f.discardPaddingNs > 0)
                last = std::max<int64_t>(0, n - std::llround(static_cast<double>(f.discardPaddingNs) * srcRate_ / 1e9));
            if (start < 0) first = std::min<int64_t>(last, -start);   // the pre-skip
            if (last <= first) continue;
            xiph::appendStereo(m.raw.data() + size_t(first) * ch, static_cast<size_t>(last - first), ch, stereo);
            srcPos = start + first;
            return true;
        }
        }
    }
    hr = E_FAIL;
    return false;
}

Decoder::Status XiphDecoder::read(std::vector<float>& out, int64_t& timestampHns, HRESULT& hr) {
    timestampHns = -1;
    hr = S_OK;
    if (!impl_) {
        hr = MF_E_NOT_INITIALIZED;
        return Decoder::Status::Error;
    }
    Impl& m = *impl_;
    for (int guard = 0; guard < 64; ++guard) {
        if (m.ended) return Decoder::Status::EndOfStream;
        m.stereo.clear();
        int64_t srcPos = -1;
        bool eos = false;
        if (!readSource(m.stereo, srcPos, eos, hr)) return Decoder::Status::Error;
        const size_t before = out.size();
        if (m.resync && srcPos >= 0) {   // a new run: the resampler restarts on the stream's output grid
            m.resync = false;
            m.resampler.reset(srcPos);
            m.outBase = m.resampler.outputStart();
            m.produced = 0;
        }
        m.resampler.process(m.stereo.data(), m.stereo.size() / 2, out);
        if (eos) {
            m.resampler.flush(out);
            m.ended = true;
        }
        const int64_t frames = static_cast<int64_t>((out.size() - before) / 2);
        if (frames > 0) {
            timestampHns = (m.outBase + m.produced) * 10'000'000 / outRate_;
            m.produced += frames;
        }
        if (eos) return Decoder::Status::EndOfStream;
        if (frames > 0) return Decoder::Status::Ok;
    }
    return Decoder::Status::Ok;
}

} // namespace st::audio
