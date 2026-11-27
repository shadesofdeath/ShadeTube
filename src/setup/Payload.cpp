// The setup's payload: pack (build host), read + verify, unpack. See Payload.h.
#include "setup/Payload.h"

#include "core/I18n.h"
#include "core/Resources.h"
#include "core/Utf.h"

#include <windows.h>
#include <bcrypt.h>
#include <compressapi.h>

#include <algorithm>
#include <cstring>
#include <format>
#include <fstream>
#include <memory>
#include <stdexcept>

#pragma comment(lib, "cabinet.lib")   // Windows Compression API (Windows 8+)
#pragma comment(lib, "bcrypt.lib")

namespace st::setup::payload {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[8] = {'S', 'T', 'S', 'E', 'T', 'U', 'P', '1'};
constexpr uint32_t kFormat = 1;
constexpr uint32_t kMaxFiles = 64, kMaxName = 128, kMaxVersion = 64;
constexpr uint64_t kMaxTotal = 1ull << 30;   // 1 GB: far beyond any ShadeTube, small enough to refuse garbage

[[noreturn]] void damaged() { throw std::runtime_error(toUtf8(tr(L"Kurulum paketi bozuk. ShadeTube'u yeniden indir."))); }
[[noreturn]] void fail(std::wstring_view message) { throw std::runtime_error(toUtf8(message)); }

void put32(std::string& s, uint32_t v) {
    for (int i = 0; i < 4; ++i) s += static_cast<char>((v >> (8 * i)) & 0xff);
}
void put64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; ++i) s += static_cast<char>((v >> (8 * i)) & 0xff);
}

// Bounds-checked little-endian reader; any read past the end is damage.
class Reader {
public:
    explicit Reader(std::string_view d) : d_(d) {}
    uint32_t u32() {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<uint8_t>(d_[pos_ + i])) << (8 * i);
        pos_ += 4;
        return v;
    }
    uint64_t u64() {
        const uint64_t lo = u32();
        return lo | static_cast<uint64_t>(u32()) << 32;
    }
    std::string_view bytes(size_t n) {
        need(n);
        const auto v = d_.substr(pos_, n);
        pos_ += n;
        return v;
    }
    size_t pos() const { return pos_; }

private:
    void need(size_t n) const {
        if (n > d_.size() || pos_ > d_.size() - n) damaged();
    }
    std::string_view d_;
    size_t pos_ = 0;
};

bool validName(std::string_view n) {
    if (n.empty() || n.size() > kMaxName || n == "." || n == "..") return false;
    return std::all_of(n.begin(), n.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

struct Compressor {
    COMPRESSOR_HANDLE h = nullptr;
    ~Compressor() {
        if (h) CloseCompressor(h);
    }
};
struct Decompressor {
    DECOMPRESSOR_HANDLE h = nullptr;
    ~Decompressor() {
        if (h) CloseDecompressor(h);
    }
};

} // namespace

// ---- SHA-256 ----------------------------------------------------------------------------------------------

Sha256::Sha256() {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
        fail(tr(L"SHA-256 kullanılamıyor."));
    if (!BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0))) {
        BCryptCloseAlgorithmProvider(alg, 0);
        fail(tr(L"SHA-256 kullanılamıyor."));
    }
    alg_ = alg;
    hash_ = hash;
}

Sha256::~Sha256() {
    if (hash_) BCryptDestroyHash(static_cast<BCRYPT_HASH_HANDLE>(hash_));
    if (alg_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(alg_), 0);
}

void Sha256::update(const void* data, size_t size) {
    const auto* p = static_cast<const uint8_t*>(data);
    while (size > 0) {   // ULONG chunks
        const ULONG n = static_cast<ULONG>(std::min<size_t>(size, 1u << 30));
        BCryptHashData(static_cast<BCRYPT_HASH_HANDLE>(hash_), const_cast<PUCHAR>(p), n, 0);
        p += n;
        size -= n;
    }
}

std::array<uint8_t, 32> Sha256::finish() {
    std::array<uint8_t, 32> out{};
    if (!BCRYPT_SUCCESS(BCryptFinishHash(static_cast<BCRYPT_HASH_HANDLE>(hash_), out.data(), 32, 0))) fail(tr(L"SHA-256 hesaplanamadı."));
    return out;
}

std::array<uint8_t, 32> sha256(std::string_view data) {
    Sha256 h;
    h.update(data.data(), data.size());
    return h.finish();
}

std::string hex(const std::array<uint8_t, 32>& digest) {
    std::string s;
    for (uint8_t b : digest) s += std::format("{:02x}", b);
    return s;
}

// ---- Pack -------------------------------------------------------------------------------------------------

std::string pack(std::string_view version, const std::vector<std::pair<std::string, fs::path>>& files, uint32_t algorithm,
                 uint32_t blockSize) {
    if (files.empty() || files.size() > kMaxFiles) throw std::runtime_error("payload: 1..64 files");
    if (version.empty() || version.size() > kMaxVersion) throw std::runtime_error("payload: bad version");
    if (blockSize < 64 * 1024 || blockSize > (64u << 20)) throw std::runtime_error("payload: block size 64 KB..64 MB");
    std::string all;   // every file, concatenated
    std::string head(kMagic, sizeof kMagic);
    put32(head, kFormat);
    put32(head, algorithm);
    put32(head, blockSize);
    std::string fileTable;
    for (const auto& [name, path] : files) {
        if (!validName(name)) throw std::runtime_error("payload: bad file name " + name);
        std::ifstream f(path, std::ios::binary);
        if (!f) throw std::runtime_error("payload: cannot read " + toUtf8(path.wstring()));
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        put32(fileTable, static_cast<uint32_t>(name.size()));
        fileTable += name;
        put64(fileTable, data.size());
        const auto digest = sha256(data);
        fileTable.append(reinterpret_cast<const char*>(digest.data()), digest.size());
        all += data;
    }
    if (all.size() > kMaxTotal) throw std::runtime_error("payload: too large");
    Compressor c;
    if (!CreateCompressor(algorithm, nullptr, &c.h)) throw std::runtime_error(std::format("payload: CreateCompressor({}) failed ({})", algorithm, GetLastError()));
    std::vector<std::string> blocks;
    for (size_t off = 0; off < all.size(); off += blockSize) {
        const size_t n = std::min<size_t>(blockSize, all.size() - off);
        SIZE_T need = 0;
        Compress(c.h, all.data() + off, n, nullptr, 0, &need);   // asks for the buffer size
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || need == 0) throw std::runtime_error(std::format("payload: Compress size query failed ({})", GetLastError()));
        std::string out(need, '\0');
        SIZE_T got = 0;
        if (!Compress(c.h, all.data() + off, n, out.data(), out.size(), &got)) throw std::runtime_error(std::format("payload: Compress failed ({})", GetLastError()));
        out.resize(got);
        blocks.push_back(std::move(out));
    }
    put32(head, static_cast<uint32_t>(blocks.size()));
    put32(head, static_cast<uint32_t>(files.size()));
    put32(head, static_cast<uint32_t>(version.size()));
    head += version;
    head += fileTable;
    for (const auto& b : blocks) put32(head, static_cast<uint32_t>(b.size()));
    for (const auto& b : blocks) head += b;
    const auto seal = sha256(head);
    head.append(reinterpret_cast<const char*>(seal.data()), seal.size());
    return head;
}

// ---- Read / unpack --------------------------------------------------------------------------------------------

Info read(std::string_view payload) {
    if (payload.size() < sizeof kMagic + 32 || std::memcmp(payload.data(), kMagic, sizeof kMagic) != 0) damaged();
    const std::string_view body = payload.substr(0, payload.size() - 32);
    const auto seal = sha256(body);
    if (std::memcmp(seal.data(), payload.data() + body.size(), 32) != 0) damaged();
    Reader r(body);
    r.bytes(sizeof kMagic);
    Info info;
    if (r.u32() != kFormat) damaged();   // the reader and the payload are built together: another format is damage
    info.algorithm = r.u32();
    info.blockSize = r.u32();
    const uint32_t blockCount = r.u32(), fileCount = r.u32(), versionLen = r.u32();
    if (info.blockSize == 0 || fileCount == 0 || fileCount > kMaxFiles || versionLen == 0 || versionLen > kMaxVersion) damaged();
    info.version = std::string(r.bytes(versionLen));
    for (uint32_t i = 0; i < fileCount; ++i) {
        File f;
        const uint32_t nameLen = r.u32();
        if (nameLen > kMaxName) damaged();
        f.name = std::string(r.bytes(nameLen));
        if (!validName(f.name)) damaged();
        f.size = r.u64();
        std::memcpy(f.sha256.data(), r.bytes(32).data(), 32);
        if (f.size > kMaxTotal || info.totalSize + f.size > kMaxTotal) damaged();
        info.totalSize += f.size;
        info.files.push_back(std::move(f));
    }
    // Exactly as many blocks as the total size needs, and their bytes fill the rest of the body.
    const uint64_t wanted = (info.totalSize + info.blockSize - 1) / info.blockSize;
    if (blockCount != wanted) damaged();
    uint64_t compressed = 0;
    for (uint32_t i = 0; i < blockCount; ++i) {
        const uint32_t n = r.u32();
        if (n == 0) damaged();
        info.blocks.push_back(n);
        compressed += n;
    }
    info.dataOffset = r.pos();
    if (compressed != body.size() - info.dataOffset) damaged();
    return info;
}

std::vector<fs::path> unpack(std::string_view payload, const fs::path& dir, const Progress& progress, const std::function<bool()>& cancelled) {
    const Info info = read(payload);
    std::error_code ec;
    fs::create_directories(dir, ec);
    Decompressor d;
    if (!CreateDecompressor(info.algorithm, nullptr, &d.h)) fail(tr(L"Bu Windows sürümü kurulum paketini açamıyor."));

    std::vector<fs::path> written;
    auto cleanup = [&] {
        for (const auto& p : written) fs::remove(p, ec);
    };
    try {
        size_t fileIndex = 0;
        uint64_t fileDone = 0, done = 0;
        std::unique_ptr<std::ofstream> out;
        std::unique_ptr<Sha256> hash;
        auto openNext = [&] {
            // Zero-length files are complete as soon as they are opened.
            while (fileIndex < info.files.size()) {
                const fs::path p = dir / toWide(info.files[fileIndex].name);
                written.push_back(p);
                out = std::make_unique<std::ofstream>(p, std::ios::binary | std::ios::trunc);
                if (!*out) fail(i18n::format(tr(L"{} yazılamadı."), {p.filename().wstring()}));
                hash = std::make_unique<Sha256>();
                fileDone = 0;
                if (info.files[fileIndex].size > 0) return;
                out.reset();
                if (hash->finish() != info.files[fileIndex].sha256) damaged();
                ++fileIndex;
            }
        };
        openNext();
        std::string block(info.blockSize, '\0');
        size_t offset = info.dataOffset;
        for (size_t b = 0; b < info.blocks.size(); ++b) {
            if (cancelled && cancelled()) fail(tr(L"Kurulum iptal edildi."));
            const uint64_t expected = std::min<uint64_t>(info.blockSize, info.totalSize - static_cast<uint64_t>(b) * info.blockSize);
            SIZE_T got = 0;
            if (!Decompress(d.h, payload.data() + offset, info.blocks[b], block.data(), block.size(), &got) || got != expected) damaged();
            offset += info.blocks[b];
            // Hand the block's bytes to the files they belong to.
            size_t pos = 0;
            while (pos < got) {
                if (fileIndex >= info.files.size() || !out) damaged();
                const File& f = info.files[fileIndex];
                const size_t n = static_cast<size_t>(std::min<uint64_t>(got - pos, f.size - fileDone));
                out->write(block.data() + pos, static_cast<std::streamsize>(n));
                hash->update(block.data() + pos, n);
                pos += n;
                fileDone += n;
                if (fileDone == f.size) {
                    out->close();
                    if (out->fail()) fail(i18n::format(tr(L"{} yazılamadı."), {toWide(f.name)}));
                    out.reset();
                    if (hash->finish() != f.sha256) damaged();
                    ++fileIndex;
                    openNext();
                }
            }
            done += got;
            if (progress) progress(done, info.totalSize);
        }
        if (fileIndex != info.files.size()) damaged();
    } catch (...) {
        cleanup();
        throw;
    }
    return written;
}

std::string_view embedded() { return res::load(L"payload"); }

} // namespace st::setup::payload
