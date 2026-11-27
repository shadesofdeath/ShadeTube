// setup_pack: build-host tool of ShadeTubeSetup.exe (src/setup/CMakeLists.txt runs it).
//   setup_pack payload <out.bin> <version> <lzms|xpress-huff|xpress|mszip> <name>=<file>...
//       the app's payload (setup/Payload.h); the first file must be an exe whose ProductVersion is <version>
//   setup_pack i18n <out dir> <assets\i18n dir> <source>...
//       the translations the setup needs: every tr(L"...") / plural(L"...") key of the given sources (the same rule as
//       tools/i18n_check.py), taken from each <code>.json into <out dir>\<code>.json
// Outputs are written every run (the build tool compares their times with the inputs').
#include "setup/Payload.h"

#include "core/Utf.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "version.lib")

namespace fs = std::filesystem;
using namespace st::setup;

namespace {

std::string readAll(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool writeIfChanged(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
    f.close();
    if (f.fail()) throw std::runtime_error("cannot write " + st::toUtf8(p.wstring()));
    return true;
}

std::string productVersion(const fs::path& exe) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (!size) return {};
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data())) return {};
    for (const wchar_t* q : {L"\\StringFileInfo\\040904b0\\ProductVersion", L"\\StringFileInfo\\040904B0\\ProductVersion"}) {
        wchar_t* value = nullptr;
        UINT len = 0;
        if (VerQueryValueW(data.data(), q, reinterpret_cast<void**>(&value), &len) && value && len > 0)
            return st::toUtf8(std::wstring_view(value, wcsnlen(value, len)));
    }
    return {};
}

// ---- i18n keys (mirrors tools/i18n_check.py: (i18n::)tr( / plural( followed by adjacent L"..." literals) ----------

bool identChar(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Reads one L"..." literal at `i` (pointing at 'L'); appends its text (escapes resolved) to `out`. False if none.
bool literal(const std::string& s, size_t& i, std::string& out) {
    if (i + 1 >= s.size() || s[i] != 'L' || s[i + 1] != '"') return false;
    size_t j = i + 2;
    while (j < s.size() && s[j] != '"') {
        if (s[j] != '\\' || j + 1 >= s.size()) {
            out += s[j++];
            continue;
        }
        const char e = s[j + 1];
        j += 2;
        auto hexRun = [&](size_t maxLen) {
            uint32_t v = 0;
            size_t n = 0;
            while (n < maxLen && j < s.size() && std::isxdigit(static_cast<unsigned char>(s[j]))) {
                v = v * 16 + static_cast<uint32_t>(std::isdigit(static_cast<unsigned char>(s[j])) ? s[j] - '0' : (std::tolower(s[j]) - 'a' + 10));
                ++j;
                ++n;
            }
            return v;
        };
        switch (e) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'u': appendUtf8(out, hexRun(4)); break;
        case 'U': appendUtf8(out, hexRun(8)); break;
        case 'x': appendUtf8(out, hexRun(8)); break;
        default: out += e; break;   // \\ \" \' \?
        }
    }
    if (j >= s.size()) return false;
    i = j + 1;
    return true;
}

void keysOf(const std::string& text, std::set<std::string>& keys) {
    for (const char* fn : {"tr(", "plural("}) {
        const size_t len = std::strlen(fn);
        for (size_t p = text.find(fn); p != std::string::npos; p = text.find(fn, p + 1)) {
            if (p > 0 && identChar(text[p - 1])) continue;
            const size_t lineStart = text.rfind('\n', p) == std::string::npos ? 0 : text.rfind('\n', p) + 1;
            size_t k = lineStart;
            while (k < p && (text[k] == ' ' || text[k] == '\t')) ++k;
            if (text.compare(k, 2, "//") == 0) continue;   // examples in comments
            size_t i = p + len;
            std::string key;
            bool any = false;
            for (;;) {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
                if (!literal(text, i, key)) break;
                any = true;
            }
            if (any) keys.insert(key);
        }
    }
}

int runI18n(int argc, wchar_t** argv) {
    const fs::path out = argv[2], dir = argv[3];
    std::set<std::string> keys;
    for (int i = 4; i < argc; ++i) keysOf(readAll(argv[i]), keys);
    int files = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        const std::wstring name = e.path().filename().wstring();
        if (e.path().extension() != L".json" || name.empty() || name[0] == L'_') continue;
        const auto all = nlohmann::json::parse(readAll(e.path()));
        nlohmann::json trimmed = nlohmann::json::object();
        size_t missing = 0;
        for (const auto& k : keys) {
            if (const auto it = all.find(k); it != all.end()) trimmed[k] = *it;
            else ++missing;
        }
        writeIfChanged(out / e.path().filename(), trimmed.dump(1, ' ', false));
        std::printf("setup_pack: %ls %zu texts%s\n", name.c_str(), trimmed.size(), missing ? " (some missing: run tools\\i18n_check.py)" : "");
        ++files;
    }
    std::printf("setup_pack: %zu keys, %d languages\n", keys.size(), files);
    return files > 0 ? 0 : 1;
}

int runPayload(int argc, wchar_t** argv) {
    const fs::path out = argv[2];
    const std::string version = st::toUtf8(argv[3]);
    const std::wstring algo = argv[4];
    const uint32_t algorithm = algo == L"lzms"          ? payload::kLzms
                               : algo == L"xpress-huff" ? payload::kXpressHuff
                               : algo == L"xpress"      ? payload::kXpress
                               : algo == L"mszip"       ? payload::kMszip
                                                        : 0;
    if (!algorithm) throw std::runtime_error("unknown algorithm");
    std::vector<std::pair<std::string, fs::path>> files;
    for (int i = 5; i < argc; ++i) {
        const std::wstring a = argv[i];
        const size_t eq = a.find(L'=');
        if (eq == std::wstring::npos) throw std::runtime_error("expected <name>=<file>");
        files.emplace_back(st::toUtf8(a.substr(0, eq)), fs::path(a.substr(eq + 1)));
    }
    if (files.empty()) throw std::runtime_error("no files");
    // The setup checks the exe against this version before installing: a mismatch here is a build error.
    if (const std::string v = productVersion(files[0].second); v != version)
        throw std::runtime_error("ProductVersion of " + files[0].first + " is \"" + v + "\", expected \"" + version + "\"");
    const auto t0 = std::chrono::steady_clock::now();
    const std::string data = payload::pack(version, files, algorithm);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    const payload::Info info = payload::read(data);   // what the setup will do first
    writeIfChanged(out, data);
    std::printf("setup_pack: %zu files, %llu -> %zu bytes (%s, %lld ms)\n", info.files.size(),
                static_cast<unsigned long long>(info.totalSize), data.size(), st::toUtf8(algo).c_str(), static_cast<long long>(ms));
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc >= 5 && std::wstring_view(argv[1]) == L"i18n") return runI18n(argc, argv);
        if (argc >= 6 && std::wstring_view(argv[1]) == L"payload") return runPayload(argc, argv);
        std::fprintf(stderr, "usage: setup_pack payload <out> <version> <algorithm> <name>=<file>...\n"
                             "       setup_pack i18n <out dir> <i18n dir> <source>...\n");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "setup_pack: %s\n", e.what());
        return 1;
    }
}
