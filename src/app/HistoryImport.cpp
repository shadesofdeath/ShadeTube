// Spotify listening-history import: JSON rows -> ListenStats::ImportRow, from the files or straight from the ZIP.
#include "app/HistoryImport.h"

#include "app/Updater.h"
#include "core/Log.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <exception>
#include <fstream>
#include <optional>
#include <stdexcept>

namespace st::app::history {

using json = nlohmann::json;
using Row = ListenStats::ImportRow;

namespace {

constexpr const char* kTag = "stats";
constexpr uint64_t kMaxDocument = 512ull << 20;   // a history JSON is ~12 MB; anything this big is not one

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
    return out;
}

std::string_view baseName(std::string_view name) {
    const size_t slash = name.find_last_of("/\\");
    return slash == std::string_view::npos ? name : name.substr(slash + 1);
}

// A string member, "" when missing or null.
std::string_view text(const json& row, const char* key) {
    const auto it = row.find(key);
    if (it == row.end() || !it->is_string()) return {};
    return it->get_ref<const std::string&>();
}

bool present(const json& row, const char* key) {
    const auto it = row.find(key);
    return it != row.end() && !it->is_null();
}

int64_t integer(const json& row, const char* key) {
    const auto it = row.find(key);
    if (it == row.end()) return -1;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    return -1;
}

int digits(std::string_view s, size_t at, size_t n) {
    if (at + n > s.size()) return -1;
    int v = 0;
    for (size_t i = at; i < at + n; ++i) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

// ---- ZIP (stored / deflate, the central directory; no ZIP64, no encryption) -----------------------------------

struct ZipEntry {
    std::string name;
    uint32_t method = 0, flags = 0, crc = 0;
    uint64_t compressed = 0, size = 0, local = 0;
};

uint32_t rd16(const std::string& b, size_t at) {
    return static_cast<uint32_t>(static_cast<unsigned char>(b[at])) | static_cast<uint32_t>(static_cast<unsigned char>(b[at + 1])) << 8;
}
uint32_t rd32(const std::string& b, size_t at) { return rd16(b, at) | rd16(b, at + 2) << 16; }

std::string readAt(std::ifstream& f, uint64_t at, uint64_t n) {
    std::string buf(static_cast<size_t>(n), '\0');
    f.clear();
    f.seekg(static_cast<std::streamoff>(at));
    f.read(buf.data(), static_cast<std::streamsize>(n));
    if (static_cast<uint64_t>(f.gcount()) != n) throw std::runtime_error("truncated ZIP");
    return buf;
}

std::vector<ZipEntry> zipEntries(std::ifstream& f, uint64_t fileSize) {
    if (fileSize < 22) throw std::runtime_error("not a ZIP file");
    const uint64_t tailSize = std::min<uint64_t>(fileSize, 22 + 0xffff);
    const std::string tail = readAt(f, fileSize - tailSize, tailSize);
    size_t eocd = std::string::npos;
    for (size_t i = tail.size() - 22;; --i) {
        if (rd32(tail, i) == 0x06054b50) {
            eocd = i;
            break;
        }
        if (i == 0) break;
    }
    if (eocd == std::string::npos) throw std::runtime_error("not a ZIP file");
    const uint64_t entries = rd16(tail, eocd + 10), cdSize = rd32(tail, eocd + 12), cdOffset = rd32(tail, eocd + 16);
    if (entries == 0xffff || cdSize == 0xffffffff || cdOffset == 0xffffffff) throw std::runtime_error("ZIP64 is not supported");
    if (cdOffset + cdSize > fileSize) throw std::runtime_error("broken ZIP directory");
    const std::string cd = readAt(f, cdOffset, cdSize);
    std::vector<ZipEntry> out;
    size_t p = 0;
    for (uint64_t i = 0; i < entries; ++i) {
        if (p + 46 > cd.size() || rd32(cd, p) != 0x02014b50) throw std::runtime_error("broken ZIP directory");
        ZipEntry e;
        e.flags = rd16(cd, p + 8);
        e.method = rd16(cd, p + 10);
        e.crc = rd32(cd, p + 16);
        e.compressed = rd32(cd, p + 20);
        e.size = rd32(cd, p + 24);
        const size_t nameLen = rd16(cd, p + 28), extraLen = rd16(cd, p + 30), commentLen = rd16(cd, p + 32);
        e.local = rd32(cd, p + 42);
        if (p + 46 + nameLen > cd.size()) throw std::runtime_error("broken ZIP directory");
        e.name = cd.substr(p + 46, nameLen);
        std::replace(e.name.begin(), e.name.end(), '\\', '/');   // PowerShell 5.1's Compress-Archive writes '\'
        out.push_back(std::move(e));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return out;
}

std::string zipRead(std::ifstream& f, uint64_t fileSize, const ZipEntry& e) {
    if (e.flags & 1) throw std::runtime_error("encrypted ZIP entries are not supported");
    if (e.size > kMaxDocument || e.compressed > kMaxDocument) throw std::runtime_error("entry too large");
    if (e.local + 30 > fileSize) throw std::runtime_error("broken ZIP entry");
    const std::string header = readAt(f, e.local, 30);
    if (rd32(header, 0) != 0x04034b50) throw std::runtime_error("broken ZIP entry");
    const uint64_t start = e.local + 30 + rd16(header, 26) + rd16(header, 28);
    if (start + e.compressed > fileSize) throw std::runtime_error("broken ZIP entry");
    std::string raw = readAt(f, start, e.compressed);
    std::string content;
    if (e.method == 0) {
        if (e.compressed != e.size) throw std::runtime_error("broken ZIP entry");
        content = std::move(raw);
    } else if (e.method == 8) {
        content = updater::inflate(raw, static_cast<size_t>(e.size));
    } else {
        throw std::runtime_error("unsupported ZIP compression");
    }
    if (updater::crc32(content.data(), content.size()) != e.crc) throw std::runtime_error("ZIP entry CRC mismatch");
    return content;
}

std::string readWhole(const std::filesystem::path& file) {
    std::ifstream f(file, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open the file");
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(file, ec);
    if (ec) throw std::runtime_error("cannot read the file");
    if (size > kMaxDocument) throw std::runtime_error("file too large");
    return readAt(f, 0, size);
}

std::string displayName(const std::filesystem::path& p) { return toUtf8(p.filename().wstring()); }

// A stable id for a song known by name only (account data, a row without a URI): "import:<FNV-1a of name + artist>".
std::string importId(std::string_view name, std::string_view artist) {
    uint64_t h = 14695981039346656037ull;
    auto mix = [&h](std::string_view s) {
        for (const char c : s) {
            h ^= static_cast<uint64_t>(std::tolower(static_cast<unsigned char>(c)));
            h *= 1099511628211ull;
        }
    };
    mix(name);
    mix("|");
    mix(artist);
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return std::string("import:") + buf;
}

} // namespace

int64_t parseUtc(std::string_view s) {
    // YYYY-MM-DD, then 'T' or ' ', HH:MM, optional :SS(.fff), optional Z / +hh:mm / -hh:mm.
    const int y = digits(s, 0, 4), mo = digits(s, 5, 2), d = digits(s, 8, 2);
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || s.size() < 16 || s[4] != '-' || s[7] != '-' ||
        (s[10] != 'T' && s[10] != ' ') || s[13] != ':')
        return -1;
    const int h = digits(s, 11, 2), mi = digits(s, 14, 2);
    if (h < 0 || h > 23 || mi < 0 || mi > 59) return -1;
    int sec = 0;
    size_t at = 16;
    if (at < s.size() && s[at] == ':') {
        sec = digits(s, at + 1, 2);
        if (sec < 0 || sec > 60) return -1;
        at += 3;
        if (at < s.size() && s[at] == '.') {
            ++at;
            while (at < s.size() && s[at] >= '0' && s[at] <= '9') ++at;
        }
    }
    int64_t offset = 0;
    if (at < s.size()) {
        if (s[at] == 'Z' || s[at] == 'z') {
            ++at;
        } else if (s[at] == '+' || s[at] == '-') {
            const int oh = digits(s, at + 1, 2);
            const size_t mAt = at + 3 < s.size() && s[at + 3] == ':' ? at + 4 : at + 3;
            const int om = digits(s, mAt, 2);
            if (oh < 0 || om < 0) return -1;
            offset = (s[at] == '+' ? 1 : -1) * (oh * 3600 + om * 60);
            at = mAt + 2;
        }
    }
    if (at != s.size()) return -1;
    return listen::daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + sec - offset;
}

bool isHistoryEntry(std::string_view name) {
    const std::string b = lower(baseName(name));
    if (b.size() < 6 || b.compare(b.size() - 5, 5, ".json") != 0) return false;
    if (b.find("video") != std::string::npos || b.find("podcast") != std::string::npos) return false;
    if (b.rfind("streaming_history_audio", 0) == 0 || b.rfind("endsong", 0) == 0 || b.rfind("streaminghistory_music", 0) == 0)
        return true;
    // Account data before 2023: StreamingHistory0.json, StreamingHistory1.json ...
    return b.rfind("streaminghistory", 0) == 0 && b.size() > 16 && std::isdigit(static_cast<unsigned char>(b[16]));
}

Format parse(std::string_view document, std::vector<Row>& out, int& skipped) {
    json j = json::parse(document, nullptr, false);
    if (j.is_discarded()) throw std::runtime_error("not a JSON file");
    if (!j.is_array()) return Format::Unknown;
    Format format = Format::Unknown;
    const size_t before = out.size();
    const int skippedBefore = skipped;
    for (const json& r : j) {
        if (!r.is_object()) continue;
        if (r.contains("ts")) {   // extended streaming history
            format = Format::Extended;
            const std::string_view name = text(r, "master_metadata_track_name");
            if (present(r, "episode_name") || present(r, "spotify_episode_uri") || present(r, "audiobook_title") ||
                present(r, "audiobook_uri") || name.empty()) {
                ++skipped;
                continue;
            }
            const int64_t ended = parseUtc(text(r, "ts"));
            const int64_t ms = integer(r, "ms_played");
            if (ended < 0 || ms < 0) {
                ++skipped;
                continue;
            }
            Row row;
            row.track.name = name;
            if (const auto artist = text(r, "master_metadata_album_artist_name"); !artist.empty()) row.track.artists.push_back({{}, std::string(artist)});
            row.track.albumName = text(r, "master_metadata_album_album_name");
            if (const auto uri = text(r, "spotify_track_uri"); uri.rfind("spotify:track:", 0) == 0) row.track.id = uri;
            else row.track.id = importId(name, row.track.artists.empty() ? std::string_view{} : row.track.artists[0].name);
            row.endedAt = ended;
            row.listenedMs = ms;
            out.push_back(std::move(row));
        } else if (r.contains("endTime")) {   // account data
            format = Format::Account;
            const std::string_view name = text(r, "trackName");
            if (present(r, "episodeName") || present(r, "podcastName") || name.empty()) {
                ++skipped;
                continue;
            }
            const int64_t ended = parseUtc(text(r, "endTime"));
            const int64_t ms = integer(r, "msPlayed");
            if (ended < 0 || ms < 0) {
                ++skipped;
                continue;
            }
            Row row;
            row.track.name = name;
            if (const auto artist = text(r, "artistName"); !artist.empty()) row.track.artists.push_back({{}, std::string(artist)});
            row.track.id = importId(name, text(r, "artistName"));
            row.endedAt = ended;
            row.listenedMs = ms;
            out.push_back(std::move(row));
        }
    }
    if (format == Format::Unknown) {   // no row looked like a history row: leave everything as it was
        out.resize(before);
        skipped = skippedBefore;
    }
    return format;
}

Result read(const std::vector<std::filesystem::path>& files, const Progress& progress, const std::atomic<bool>* cancel) {
    Result res;
    // First the plan: every JSON file, and the history entries of every ZIP (so progress has a total).
    struct Doc {
        size_t file;
        std::optional<ZipEntry> entry;
    };
    std::vector<Doc> docs;
    std::vector<std::vector<ZipEntry>> zips(files.size());
    for (size_t i = 0; i < files.size(); ++i) {
        const std::string ext = lower(toUtf8(files[i].extension().wstring()));
        try {
            if (ext == ".json") {
                docs.push_back({i, std::nullopt});
            } else if (ext == ".zip") {
                std::ifstream f(files[i], std::ios::binary);
                if (!f) throw std::runtime_error("cannot open the file");
                const uint64_t size = std::filesystem::file_size(files[i]);
                int found = 0;
                for (auto& e : zipEntries(f, size))
                    if (isHistoryEntry(e.name)) {
                        docs.push_back({i, std::move(e)});
                        ++found;
                    }
                if (!found) ++res.unrecognized;
            } else {
                throw std::runtime_error("not a .json or .zip file");
            }
        } catch (const std::exception& e) {
            res.errors.push_back(displayName(files[i]) + ": " + e.what());
        }
    }
    if (progress) progress(0, static_cast<int>(docs.size()));
    int done = 0;
    for (const auto& d : docs) {
        if (cancel && cancel->load()) {
            res.cancelled = true;
            break;
        }
        const auto& file = files[d.file];
        const std::string label = d.entry ? displayName(file) + "/" + std::string(baseName(d.entry->name)) : displayName(file);
        try {
            std::string content;
            if (d.entry) {
                std::ifstream f(file, std::ios::binary);
                if (!f) throw std::runtime_error("cannot open the file");
                content = zipRead(f, std::filesystem::file_size(file), *d.entry);
            } else {
                content = readWhole(file);
            }
            const size_t before = res.rows.size();
            const Format format = parse(content, res.rows, res.skippedRows);
            if (format == Format::Unknown) ++res.unrecognized;
            else ++res.documents;
            ST_LOG_INFO(kTag, "import: {} -> {} rows", label, res.rows.size() - before);
        } catch (const std::exception& e) {
            res.errors.push_back(label + ": " + e.what());
        }
        if (progress) progress(++done, static_cast<int>(docs.size()));
    }
    for (const auto& e : res.errors) ST_LOG_WARN(kTag, "import: {}", e);
    return res;
}

} // namespace st::app::history
