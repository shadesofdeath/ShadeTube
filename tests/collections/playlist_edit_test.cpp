// playlist_edit_test: checks for app/PlaylistEditing (playlist editing logic without UI or network).
//
//   reorder : moving a multi-selection in front of a row (order, new positions, the anchor Spotify gets), incl. a
//             randomized check that the moved block always sits right next to its anchor row
//   restore : the runs an undo puts back and where, and that re-inserting them rebuilds the old list
//   cover   : a picked image -> centered square JPEG within the size limit (noise PNG = worst case for JPEG, a
//             small portrait that is never scaled up, a tight budget, a file on disk, garbage)
//   local   : a local playlist's library.json entry round trip (order, description, custom cover, old entries
//             without one) and the cover files (new name per save, delete only inside the cover folder)
//
// Temporary files go to a per-process temp folder.
#include "app/PlaylistEditing.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using namespace st::app::pledit;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

static int g_failures = 0;
static int g_checks = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        ++g_checks;                                                                                                \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

using V = std::vector<int>;

// ---- reorder -------------------------------------------------------------------------------------------------------

static void testReorder() {
    std::printf("reorder\n");
    CHECK(moveOrder(5, {1, 3}, 0) == (V{1, 3, 0, 2, 4}));
    CHECK(moveOrder(5, {0, 1}, 5) == (V{2, 3, 4, 0, 1}));   // to the end
    CHECK(moveOrder(5, {3, 1}, 3) == (V{0, 2, 1, 3, 4}));   // picked in any order: they keep the list's order
    CHECK(moveOrder(5, {4}, 1) == (V{0, 4, 1, 2, 3}));
    CHECK(moveOrder(5, {0}, 3) == (V{1, 2, 0, 3, 4}));      // down: in front of the old row 3
    CHECK(isIdentity(moveOrder(5, {1, 2}, 2)));             // dropped inside the block itself
    CHECK(isIdentity(moveOrder(5, {1, 2}, 3)));             // right after it
    CHECK(isIdentity(moveOrder(5, {}, 2)));
    CHECK(moveOrder(3, {1, 1, 7, -1}, 0) == (V{1, 0, 2}));  // duplicates / out of range ignored
    CHECK(moveOrder(3, {2}, 99) == (V{0, 1, 2}));           // clamped to the end
    CHECK(moveOrder(0, {0}, 0).empty());
    CHECK(!isIdentity(V{1, 0}));

    CHECK(newPositions(moveOrder(5, {1, 3}, 0), {1, 3}) == (V{0, 1}));
    CHECK(newPositions(moveOrder(5, {0, 1}, 5), {0, 1}) == (V{3, 4}));

    const std::vector<std::string> rows{"a", "b", "c", "d", "e"};
    CHECK(applyOrder(rows, moveOrder(5, {3, 4}, 1)) == (std::vector<std::string>{"a", "d", "e", "b", "c"}));

    auto anchorIs = [](Anchor a, Anchor::Kind k, int row) { return a.kind == k && a.row == row; };
    CHECK(anchorIs(moveAnchor(5, {1, 3}, 0), Anchor::Kind::Before, 0));
    CHECK(anchorIs(moveAnchor(5, {0, 1}, 5), Anchor::Kind::After, 4));
    CHECK(anchorIs(moveAnchor(5, {1, 2}, 2), Anchor::Kind::Before, 3));   // the first row at / after that stays
    CHECK(anchorIs(moveAnchor(5, {3, 4}, 5), Anchor::Kind::After, 2));
    CHECK(anchorIs(moveAnchor(5, {3, 4}, 4), Anchor::Kind::After, 2));
    CHECK(moveAnchor(2, {0, 1}, 1).kind == Anchor::Kind::None);           // everything picked

    // Randomized: the moved block is contiguous, in list order, and right before / after its anchor row.
    std::mt19937 rng(1234);
    int bad = 0;
    for (int round = 0; round < 3000; ++round) {
        const int n = 1 + static_cast<int>(rng() % 40);
        V picked;
        for (int i = 0; i < n; ++i)
            if (rng() % 4 == 0) picked.push_back(i);
        if (picked.empty()) picked.push_back(static_cast<int>(rng() % n));
        std::shuffle(picked.begin(), picked.end(), rng);
        const int before = static_cast<int>(rng() % (n + 1));
        const V order = moveOrder(n, picked, before);
        V sorted = picked;
        std::sort(sorted.begin(), sorted.end());
        V perm = order;
        std::sort(perm.begin(), perm.end());
        bool ok = static_cast<int>(order.size()) == n;
        for (int i = 0; ok && i < n; ++i) ok = perm[i] == i;   // a permutation
        const V pos = newPositions(order, picked);
        ok = ok && pos.size() == sorted.size();
        for (size_t k = 0; ok && k < pos.size(); ++k)
            ok = order[pos[k]] == sorted[k] && (k == 0 || pos[k] == pos[k - 1] + 1);   // contiguous, in order
        const Anchor a = moveAnchor(n, picked, before);
        if (ok && a.kind == Anchor::Kind::Before) ok = pos.back() + 1 < n && order[pos.back() + 1] == a.row;
        else if (ok && a.kind == Anchor::Kind::After) ok = pos.front() > 0 && order[pos.front() - 1] == a.row;
        else if (ok) ok = static_cast<int>(pos.size()) == n;
        if (!ok) ++bad;
    }
    CHECK(bad == 0);
}

// ---- restore (undo of a removal) -----------------------------------------------------------------------------------

static void testRestore() {
    std::printf("restore\n");
    auto runs = restoreRuns(6, {5, 1, 2});
    CHECK(runs.size() == 2);
    if (runs.size() == 2) {
        CHECK(runs[0].rows == (V{1, 2}));
        CHECK(runs[0].anchor.kind == Anchor::Kind::Before && runs[0].anchor.row == 3);
        CHECK(runs[1].rows == (V{5}));
        CHECK(runs[1].anchor.kind == Anchor::Kind::After && runs[1].anchor.row == 4);
    }
    runs = restoreRuns(3, {0, 1, 2});
    CHECK(runs.size() == 1 && runs[0].anchor.kind == Anchor::Kind::None);
    CHECK(restoreRuns(3, {}).empty());

    // Removing, then re-inserting each run at its anchor (what the Spotify undo does) gives the old list back.
    std::mt19937 rng(99);
    int bad = 0;
    for (int round = 0; round < 2000; ++round) {
        const int n = 1 + static_cast<int>(rng() % 30);
        V removed;
        for (int i = 0; i < n; ++i)
            if (rng() % 3 == 0) removed.push_back(i);
        std::vector<int> list;   // the rows left (old indices)
        for (int i = 0; i < n; ++i)
            if (std::find(removed.begin(), removed.end(), i) == removed.end()) list.push_back(i);
        for (const auto& run : restoreRuns(n, removed)) {
            auto at = list.end();
            if (run.anchor.kind == Anchor::Kind::Before) at = std::find(list.begin(), list.end(), run.anchor.row);
            else if (run.anchor.kind == Anchor::Kind::After) at = std::find(list.begin(), list.end(), run.anchor.row) + 1;
            else at = list.begin();
            list.insert(at, run.rows.begin(), run.rows.end());
        }
        V all(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) all[static_cast<size_t>(i)] = i;
        if (list != all) ++bad;
    }
    CHECK(bad == 0);

    // The local undo: re-insert at the old indices.
    std::vector<std::string> rows{"a", "c", "e"};
    reinsert(rows, std::vector<std::pair<int, std::string>>{{3, "d"}, {1, "b"}});
    CHECK(rows == (std::vector<std::string>{"a", "b", "c", "d", "e"}));
    rows = {"a"};
    reinsert(rows, std::vector<std::pair<int, std::string>>{{9, "z"}});   // past the end: appended
    CHECK(rows == (std::vector<std::string>{"a", "z"}));
}

// ---- cover ---------------------------------------------------------------------------------------------------------

// A PNG (w x h) of random pixels (`noise`) or a smooth gradient, encoded with WIC.
static std::vector<uint8_t> makePng(int w, int h, bool noise) {
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return {};
    std::vector<BYTE> px(static_cast<size_t>(w) * h * 4);
    std::mt19937 rng(7);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            BYTE* p = &px[(static_cast<size_t>(y) * w + x) * 4];
            p[0] = noise ? static_cast<BYTE>(rng()) : static_cast<BYTE>(x * 255 / w);
            p[1] = noise ? static_cast<BYTE>(rng()) : static_cast<BYTE>(y * 255 / h);
            p[2] = noise ? static_cast<BYTE>(rng()) : 128;
            p[3] = 255;
        }
    ComPtr<IWICBitmap> bmp;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4, static_cast<UINT>(px.size()),
                                           px.data(), &bmp)) ||
        FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(bmp.Get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(enc->Commit()))
        return {};
    STATSTG st{};
    stream->Stat(&st, STATFLAG_NONAME);
    std::vector<uint8_t> out(static_cast<size_t>(st.cbSize.QuadPart));
    const LARGE_INTEGER zero{};
    ULONG read = 0;
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    stream->Read(out.data(), static_cast<ULONG>(out.size()), &read);
    return out;
}

static bool isJpeg(const std::vector<uint8_t>& b) { return b.size() > 4 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF; }

static void testCover(const fs::path& dir) {
    std::printf("cover\n");
    // Noise: the worst case for JPEG (a 640 px square of it is ~1 MB at high quality).
    const auto noise = makePng(1200, 800, true);
    CHECK(!noise.empty());
    auto c = squareJpeg(noise.data(), noise.size());
    CHECK(isJpeg(c.jpeg));
    CHECK(c.jpeg.size() <= kMaxCoverBytes);
    CHECK(c.side > 0 && c.side <= kCoverSide);
    auto [w, h] = imageSize(c.jpeg.data(), c.jpeg.size());
    CHECK(w == c.side && h == c.side);
    std::printf("  noise 1200x800 -> %d px, %zu bytes\n", c.side, c.jpeg.size());

    // A smooth picture fits at full size; a small one is never scaled up.
    const auto smooth = makePng(1600, 900, false);
    c = squareJpeg(smooth.data(), smooth.size());
    CHECK(c.side == kCoverSide && c.jpeg.size() <= kMaxCoverBytes);
    const auto portrait = makePng(300, 500, false);
    c = squareJpeg(portrait.data(), portrait.size());
    CHECK(c.side == 300);
    std::tie(w, h) = imageSize(c.jpeg.data(), c.jpeg.size());
    CHECK(w == 300 && h == 300);

    // A tight budget: smaller and / or lower quality, still within it.
    c = squareJpeg(noise.data(), noise.size(), kCoverSide, 24 * 1024);
    CHECK(isJpeg(c.jpeg) && c.jpeg.size() <= 24 * 1024 && c.side < kCoverSide);

    // From a file (non-ASCII path), and things that are no picture.
    const fs::path file = dir / L"kapak görseli.png";
    {
        std::ofstream f(file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(noise.data()), static_cast<std::streamsize>(noise.size()));
    }
    c = squareJpeg(file);
    CHECK(isJpeg(c.jpeg) && c.jpeg.size() <= kMaxCoverBytes);
    CHECK(squareJpeg(dir / L"missing.png").jpeg.empty());
    const std::string garbage = "not an image at all";
    CHECK(squareJpeg(reinterpret_cast<const uint8_t*>(garbage.data()), garbage.size()).jpeg.empty());
    CHECK(squareJpeg(nullptr, 0).jpeg.empty());
}

// ---- local playlists -----------------------------------------------------------------------------------------------

static void testLocal(const fs::path& dir) {
    std::printf("local\n");
    LocalPlaylist p;
    p.meta.id = "local:00ff00ff00ff00ff";
    p.meta.name = "Yol şarkıları";
    p.meta.description = "Uzun yollar için — \"en iyiler\"";
    p.meta.createdAt = 1700000000;
    for (int i = 0; i < 5; ++i) {
        st::catalog::Track t;
        t.id = "spotify:track:" + std::to_string(i);
        t.name = "Song " + std::to_string(i);
        t.addedAt = 1700000100 + i;
        p.tracks.push_back(t);
    }
    p.tracks = applyOrder(p.tracks, moveOrder(5, {3, 4}, 0));   // the user's order
    const std::vector<uint8_t> jpeg{0xFF, 0xD8, 0xFF, 0xE0, 1, 2, 3, 4, 0xFF, 0xD9};
    const fs::path covers = dir / L"playlist-covers";
    p.cover = saveCover(covers, p.meta.id, jpeg);
    CHECK(!p.cover.empty() && fs::exists(p.cover));
    CHECK(fs::path(p.cover).parent_path() == covers);
    CHECK(fs::path(p.cover).filename().wstring().find(L':') == std::wstring::npos);

    // Through library.json text and back.
    const std::string text = toJson(p).dump();
    LocalPlaylist q;
    CHECK(fromJson(nlohmann::json::parse(text), q));
    CHECK(q.meta.id == p.meta.id && q.meta.name == p.meta.name && q.meta.description == p.meta.description);
    CHECK(q.meta.createdAt == p.meta.createdAt);
    CHECK(q.cover == p.cover);
    CHECK(q.tracks.size() == 5);
    if (q.tracks.size() == 5) {
        CHECK(q.tracks[0].id == "spotify:track:3" && q.tracks[1].id == "spotify:track:4" && q.tracks[2].id == "spotify:track:0");
        CHECK(q.tracks[4].addedAt == 1700000102 && q.tracks[0].name == "Song 3");
    }
    // Written before custom covers existed: no "cover" key; and an entry without an id is skipped.
    CHECK(fromJson(nlohmann::json::parse(R"({"id":"local:1","n":"Eski","desc":"","c":5,"tracks":[]})"), q));
    CHECK(q.cover.empty() && q.meta.name == "Eski" && q.tracks.empty());
    CHECK(!fromJson(nlohmann::json::parse(R"({"n":"no id"})"), q));
    CHECK(!fromJson(nlohmann::json::parse("[]"), q));
    CHECK(toJson(q).dump().find("cover") == std::string::npos);

    // The file holds the picture; a new save gets a new name (no stale cached image); delete only inside the folder.
    std::ifstream in(p.cover, std::ios::binary);
    const std::vector<uint8_t> back((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    CHECK(back == jpeg);
    const std::wstring second = saveCover(covers, p.meta.id, jpeg);
    CHECK(!second.empty() && second != p.cover);
    deleteCover(covers, p.cover);
    CHECK(!fs::exists(p.cover) && fs::exists(second));
    const fs::path outside = dir / L"user-picture.jpg";
    {
        std::ofstream f(outside, std::ios::binary);
        f << "keep me";
    }
    deleteCover(covers, outside.wstring());
    CHECK(fs::exists(outside));
    CHECK(saveCover(covers, p.meta.id, {}).empty());
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IONBF, 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const fs::path dir = fs::path(tmp) / (L"shadetube_playlist_edit_test_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::create_directories(dir, ec);

    testReorder();
    testRestore();
    testCover(dir);
    testLocal(dir);

    fs::remove_all(dir, ec);
    CoUninitialize();
    if (g_failures) {
        std::printf("\n%d of %d check(s) FAILED\n", g_failures, g_checks);
        return 1;
    }
    std::printf("\nall %d playlist editing checks passed\n", g_checks);
    return 0;
}
