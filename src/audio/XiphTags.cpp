#include "audio/XiphTags.h"

#include <opusfile.h>
#include <vorbis/vorbisfile.h>

#include <cstdio>
#include <cstring>
#include <memory>

namespace st::audio::xiph {

namespace {

int fRead(void* s, unsigned char* p, int n) {
    return n <= 0 ? 0 : static_cast<int>(fread(p, 1, static_cast<size_t>(n), static_cast<FILE*>(s)));
}
int fSeek(void* s, opus_int64 offset, int whence) { return _fseeki64(static_cast<FILE*>(s), offset, whence) == 0 ? 0 : -1; }
opus_int64 fTell(void* s) { return _ftelli64(static_cast<FILE*>(s)); }

size_t vRead(void* p, size_t size, size_t n, void* s) { return fread(p, size, n, static_cast<FILE*>(s)); }
int vSeek(void* s, ogg_int64_t offset, int whence) { return _fseeki64(static_cast<FILE*>(s), offset, whence) == 0 ? 0 : -1; }
long vTell(void* s) { return static_cast<long>(_ftelli64(static_cast<FILE*>(s))); }

struct FileCloser {
    void operator()(FILE* f) const {
        if (f) fclose(f);
    }
};

// "KEY=value" comments (binary safe) -> (upper-case KEY, value).
void addComments(char** comments, const int* lengths, int count, FileInfo& out) {
    for (int i = 0; i < count; ++i) {
        if (!comments[i] || lengths[i] <= 0) continue;
        const std::string_view entry(comments[i], static_cast<size_t>(lengths[i]));
        const size_t eq = entry.find('=');
        if (eq == std::string_view::npos || eq == 0) continue;
        std::string key(entry.substr(0, eq));
        for (char& c : key)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        out.tags.emplace_back(std::move(key), std::string(entry.substr(eq + 1)));
    }
}

// The front cover (else the first picture) among the METADATA_BLOCK_PICTURE tags.
std::string pickPicture(const FileInfo& info) {
    std::string best;
    bool front = false;
    for (const auto& [key, value] : info.tags) {
        if (key != "METADATA_BLOCK_PICTURE" || front) continue;
        OpusPictureTag pic;
        opus_picture_tag_init(&pic);
        if (opus_picture_tag_parse(&pic, value.c_str()) == 0 && pic.format != OP_PIC_FORMAT_URL && pic.data && pic.data_length > 0 &&
            (best.empty() || pic.type == 3)) {
            best.assign(reinterpret_cast<const char*>(pic.data), pic.data_length);
            front = pic.type == 3;
        }
        opus_picture_tag_clear(&pic);
    }
    return best;
}

} // namespace

std::string FileInfo::tag(std::string_view key) const {
    for (const auto& [k, v] : tags)
        if (k == key) return v;
    return {};
}

std::vector<std::string> FileInfo::tagValues(std::string_view key) const {
    std::vector<std::string> out;
    for (const auto& [k, v] : tags)
        if (k == key) out.push_back(v);
    return out;
}

std::optional<FileInfo> readOggFile(const std::filesystem::path& path, bool withPicture) {
    std::unique_ptr<FILE, FileCloser> f(_wfopen(path.c_str(), L"rb"));
    if (!f) return std::nullopt;
    char magic[4] = {};
    if (fread(magic, 1, 4, f.get()) != 4 || std::memcmp(magic, "OggS", 4) != 0) return std::nullopt;
    FileInfo info;
    _fseeki64(f.get(), 0, SEEK_SET);
    static const OpusFileCallbacks kOpus = {fRead, fSeek, fTell, nullptr};
    int err = 0;
    if (OggOpusFile* of = op_open_callbacks(f.get(), &kOpus, nullptr, 0, &err)) {
        info.codec = "Opus";
        if (const OpusHead* head = op_head(of, 0)) {
            info.channels = static_cast<uint32_t>(head->channel_count);
            info.sampleRate = head->input_sample_rate;
        }
        const ogg_int64_t total = op_pcm_total(of, -1);
        if (total > 0) info.durationMs = total / 48;
        if (const OpusTags* tags = op_tags(of, 0)) addComments(tags->user_comments, tags->comment_lengths, tags->comments, info);
        op_free(of);
    } else {
        _fseeki64(f.get(), 0, SEEK_SET);
        static const ov_callbacks kVorbis = {vRead, vSeek, nullptr, vTell};
        OggVorbis_File vf;
        if (ov_open_callbacks(f.get(), &vf, nullptr, 0, kVorbis) != 0) return std::nullopt;
        info.codec = "Vorbis";
        if (const vorbis_info* vi = ov_info(&vf, 0)) {
            info.channels = static_cast<uint32_t>(vi->channels);
            info.sampleRate = static_cast<uint32_t>(vi->rate);
        }
        const double total = ov_time_total(&vf, -1);
        if (total > 0) info.durationMs = static_cast<int64_t>(total * 1000 + 0.5);
        if (vorbis_comment* vc = ov_comment(&vf, 0)) addComments(vc->user_comments, vc->comment_lengths, vc->comments, info);
        ov_clear(&vf);
    }
    if (withPicture) info.picture = pickPicture(info);
    // The pictures are large and useless to callers as text.
    std::erase_if(info.tags, [](const auto& t) { return t.first == "METADATA_BLOCK_PICTURE" || t.first == "COVERART"; });
    return info;
}

} // namespace st::audio::xiph
