#pragma once
// The app inside the setup: ShadeTube.exe and its license files, compressed with the Windows Compression API
// (cabinet.dll; LZMS for releases, XPRESS_HUFF for quick Debug builds) and embedded in ShadeTubeSetup.exe as the
// RCDATA resource "payload" (written at build time by setup_pack, src/setup/Pack.cpp).
//
// Layout (little endian):
//   "STSETUP1"  u32 format (1)  u32 algorithm (COMPRESS_ALGORITHM_*)  u32 block size  u32 block count  u32 file count
//   u32 version length + the app version (UTF-8, "0.12.0")
//   per file:  u32 name length + name (UTF-8, a plain file name)  u64 size  32 bytes SHA-256 of the file
//   per block: u32 compressed size
//   the blocks: the files' bytes concatenated, cut into blocks of `block size` (the last one shorter), each compressed
//               on its own (Compress buffer mode), so unpacking reports progress per block and needs one block of memory
//   32 bytes SHA-256 of everything before it (the seal)
// read() checks the seal and the structure; unpack() decompresses block by block and checks every file's SHA-256 and
// size before it counts as written. Nothing is trusted before the seal matches.
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace st::setup::payload {

inline constexpr uint32_t kMszip = 2, kXpress = 3, kXpressHuff = 4, kLzms = 5;   // COMPRESS_ALGORITHM_*
inline constexpr uint32_t kDefaultBlock = 4u << 20;

struct File {
    std::string name;   // "ShadeTube.exe"
    uint64_t size = 0;
    std::array<uint8_t, 32> sha256{};
};

struct Info {
    std::string version;       // the app's version, as the packer was told (== the exe's ProductVersion)
    uint32_t algorithm = 0, blockSize = 0;
    std::vector<File> files;
    std::vector<uint32_t> blocks;   // compressed size of each block
    uint64_t totalSize = 0;         // sum of the file sizes
    size_t dataOffset = 0;          // where the first block starts
};

// SHA-256 (bcrypt) of a buffer, incremental.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, size_t size);
    std::array<uint8_t, 32> finish();

private:
    void* alg_ = nullptr;
    void* hash_ = nullptr;
};
std::array<uint8_t, 32> sha256(std::string_view data);
std::string hex(const std::array<uint8_t, 32>& digest);

// Builds a payload from (name, file) pairs. Names must be plain file names ([A-Za-z0-9._-], no folders). Throws
// std::runtime_error.
std::string pack(std::string_view version, const std::vector<std::pair<std::string, std::filesystem::path>>& files,
                  uint32_t algorithm = kLzms, uint32_t blockSize = kDefaultBlock);

// Parses and verifies (seal, structure, names, sizes). Throws std::runtime_error (UI language) on any damage.
Info read(std::string_view payload);

// Unpacks every file into `dir` (created if needed) under its own name, verifying each one; a file that fails is
// deleted again. `progress(done, total)` gets the unpacked bytes so far. `cancelled()` (optional) is asked between
// blocks. Throws std::runtime_error (UI language). Returns the written paths, in payload order.
using Progress = std::function<void(uint64_t done, uint64_t total)>;
std::vector<std::filesystem::path> unpack(std::string_view payload, const std::filesystem::path& dir, const Progress& progress = {},
                                          const std::function<bool()>& cancelled = {});

// The payload embedded in this exe (RCDATA "payload"); empty when there is none.
std::string_view embedded();

} // namespace st::setup::payload
