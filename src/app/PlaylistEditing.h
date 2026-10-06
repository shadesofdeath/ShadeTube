#pragma once
// Playlist editing logic that needs neither the UI nor the network (tests/collections compiles this file directly):
//   * reordering: moving a (multi-)selection of rows in front of another row, and where Spotify must put them
//     (its moveItemsInPlaylist / addToPlaylist take a position relative to a row uid, not an index);
//   * undoing a removal: where each run of removed rows goes back;
//   * cover images: a picked image file -> a centered square JPEG small enough for Spotify's upload (WIC);
//   * local playlists: their library.json entry (name, description, custom cover, songs in order) and the cover files.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace st::app::pledit {

// ---- Reordering ------------------------------------------------------------------------------------------------

// Moves the rows `picked` (indices into a list of `count` rows; any order, duplicates and out-of-range indices are
// ignored) so they sit together, in their current relative order, in front of the row that was at `insertBefore`
// (0..count; count = the end). Returns the new order: result[k] = the old index of the row now at k.
std::vector<int> moveOrder(int count, const std::vector<int>& picked, int insertBefore);
bool isIdentity(const std::vector<int>& order);

// The rows in `order` (from moveOrder).
template <class T>
std::vector<T> applyOrder(const std::vector<T>& rows, const std::vector<int>& order) {
    std::vector<T> out;
    out.reserve(order.size());
    for (int i : order) out.push_back(rows[static_cast<size_t>(i)]);
    return out;
}
// Where the rows `picked` ended up in `order` (new indices, ascending).
std::vector<int> newPositions(const std::vector<int>& order, const std::vector<int>& picked);

// A position relative to a row that stays where it is: in front of `row`, right after it, or None (no row stays:
// the moved / restored rows are the whole list).
struct Anchor {
    enum class Kind { None, Before, After };
    Kind kind = Kind::None;
    int row = -1;   // old index
};
// Where moveOrder(count, picked, insertBefore) puts the picked rows, relative to a row that is not picked: in front
// of the first unpicked row at or after `insertBefore`, else right after the last unpicked row before it.
Anchor moveAnchor(int count, const std::vector<int>& picked, int insertBefore);

// Undo of a removal: each run of consecutive removed rows (old indices, ascending) and where it goes back — in front
// of the next row that stayed, else after the previous one.
struct Run {
    std::vector<int> rows;
    Anchor anchor;
};
std::vector<Run> restoreRuns(int count, const std::vector<int>& removed);

// Puts removed rows back at their old indices (`removed`: old index + row, any order) into what is left.
template <class T>
void reinsert(std::vector<T>& rows, std::vector<std::pair<int, T>> removed) {
    std::sort(removed.begin(), removed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& [at, row] : removed) {
        const size_t i = std::min(static_cast<size_t>(std::max(at, 0)), rows.size());
        rows.insert(rows.begin() + static_cast<std::ptrdiff_t>(i), std::move(row));
    }
}

// ---- Cover images ----------------------------------------------------------------------------------------------

inline constexpr size_t kMaxCoverBytes = 256 * 1024;   // Spotify's limit for a playlist cover (JPEG)
inline constexpr int kCoverSide = 640;                 // the largest size Spotify shows

struct Cover {
    std::vector<uint8_t> jpeg;   // empty on failure
    int side = 0;                // width = height of the encoded image
};
// Decodes an image (whatever WIC reads: JPEG, PNG, WebP, BMP, GIF...), crops the centered square, scales it down to
// at most `maxSide` and encodes a JPEG of at most `maxBytes` (the quality steps down first, then the size). Sets up
// COM for the calling thread when it has none (worker threads).
Cover squareJpeg(const std::filesystem::path& file, int maxSide = kCoverSide, size_t maxBytes = kMaxCoverBytes);
Cover squareJpeg(const uint8_t* data, size_t size, int maxSide = kCoverSide, size_t maxBytes = kMaxCoverBytes);
// Width and height of an encoded image (0, 0 when WIC can't read it). For checks.
std::pair<int, int> imageSize(const uint8_t* data, size_t size);

// ---- Local playlists -------------------------------------------------------------------------------------------

// A local playlist as library.json stores it.
struct LocalPlaylist {
    catalog::Playlist meta;               // id, name, description, createdAt (images / count are derived)
    std::vector<catalog::Track> tracks;   // in the user's order
    std::wstring cover;                   // custom cover file, "" = the first songs' artwork
};
nlohmann::json toJson(const LocalPlaylist& p);
// False when the entry has no id.
bool fromJson(const nlohmann::json& j, LocalPlaylist& out);

// Custom covers of local playlists: <dir>\<id>-<n>.jpg (a new name per change, so a cached image of the old one is
// never shown). Returns the file written, "" on failure.
std::wstring saveCover(const std::filesystem::path& dir, const std::string& playlistId, const std::vector<uint8_t>& jpeg);
// Deletes a cover written by saveCover (anything outside `dir` is left alone).
void deleteCover(const std::filesystem::path& dir, const std::wstring& file);

} // namespace st::app::pledit
