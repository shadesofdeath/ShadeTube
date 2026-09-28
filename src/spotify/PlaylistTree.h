#pragma once
// Folders of the user's Spotify library. The rootlist (Api::rootlist) is a flat list in which folders are brackets:
//   spotify:start-group:<group id>:<form-encoded name>   ...playlists and nested groups...   spotify:end-group:<group id>
// ("+" for spaces, %XX for the rest), nested to any depth. libraryV3, which gives the sidebar its order, carries no
// membership, so the tree comes from the rootlist and the order from libraryV3 (treeRows).
// Standalone (catalog + nlohmann/json): tests/spotify compiles it with the rest of st_spotify.
#include "catalog/Models.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace st::spotify {

struct PlaylistFolder {
    std::string id;           // group id from the rootlist (stable; the web API's spotify:user:<u>:folder:<id>)
    std::string name;         // decoded; may be empty
    std::string parentId;     // "" = top level
    int playlistCount = 0;    // playlists inside, nested folders included
};

struct PlaylistTree {
    std::vector<PlaylistFolder> folders;                     // rootlist order (a parent before its children)
    std::unordered_map<std::string, std::string> folderOf;   // playlist URI -> folder id (top level: absent)

    bool empty() const { return folders.empty(); }
    const PlaylistFolder* folder(const std::string& id) const;
};

// Parses a rootlist response (contents.items[].uri). Unbalanced markers are tolerated: an end without its start
// closes nothing, groups left open end with the list. A playlist listed twice keeps its first folder.
PlaylistTree parsePlaylistTree(const nlohmann::json& rootlist);
// "Chill+%26+Focus" -> "Chill & Focus" (form decoding; a broken escape is kept as is).
std::string decodeGroupName(std::string_view encoded);

struct TreeRow {
    bool folder = false;
    int depth = 0;            // 0 = directly under `root`
    size_t index = 0;         // into `playlists` (folder == false) or tree.folders (folder == true)
};
// The rows under folder `root` ("" = the whole library) in display order. Playlists keep the order of `playlists`
// (libraryV3's) inside their folder; a folder sits where its first playlist in that order would, so a recently used
// folder rises like a recently used playlist; empty folders go last at their level, in rootlist order. A folder's
// contents follow it only when expanded(folder id) is true (null: nothing is expanded). Playlists whose folder is
// unknown to the tree are listed at the top level.
std::vector<TreeRow> treeRows(const std::vector<catalog::Playlist>& playlists, const PlaylistTree& tree,
                              const std::function<bool(const std::string& folderId)>& expanded,
                              const std::string& root = {});

} // namespace st::spotify
