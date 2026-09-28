#include "spotify/PlaylistTree.h"

#include <algorithm>
#include <cstdint>

namespace st::spotify {

using nlohmann::json;

namespace {
constexpr std::string_view kStartGroup = "spotify:start-group:";
constexpr std::string_view kEndGroup = "spotify:end-group:";
constexpr std::string_view kPlaylist = "spotify:playlist:";

int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
} // namespace

const PlaylistFolder* PlaylistTree::folder(const std::string& id) const {
    for (const auto& f : folders)
        if (f.id == id) return &f;
    return nullptr;
}

std::string decodeGroupName(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += c;
        }
    }
    return out;
}

PlaylistTree parsePlaylistTree(const json& rootlist) {
    PlaylistTree tree;
    if (!rootlist.is_object()) return tree;
    const auto contents = rootlist.find("contents");
    if (contents == rootlist.end() || !contents->is_object()) return tree;
    const auto items = contents->find("items");
    if (items == contents->end() || !items->is_array()) return tree;

    // Open groups: indexes into tree.folders; kNone for a group that isn't a folder of its own (no id, or an id seen
    // before), whose contents stay in the enclosing folder.
    constexpr size_t kNone = SIZE_MAX;
    std::vector<size_t> stack;
    auto innermost = [&]() -> std::string {
        for (auto s = stack.rbegin(); s != stack.rend(); ++s)
            if (*s != kNone) return tree.folders[*s].id;
        return {};
    };
    for (const auto& it : *items) {
        if (!it.is_object()) continue;
        const auto u = it.find("uri");
        if (u == it.end() || !u->is_string()) continue;
        const std::string& uri = u->get_ref<const std::string&>();
        if (uri.rfind(kStartGroup, 0) == 0) {
            const std::string_view rest = std::string_view(uri).substr(kStartGroup.size());
            const size_t colon = rest.find(':');
            PlaylistFolder f;
            f.id = std::string(rest.substr(0, colon));
            if (f.id.empty() || tree.folder(f.id)) {
                stack.push_back(kNone);
                continue;
            }
            if (colon != std::string_view::npos) f.name = decodeGroupName(rest.substr(colon + 1));
            f.parentId = innermost();
            tree.folders.push_back(std::move(f));
            stack.push_back(tree.folders.size() - 1);
        } else if (uri.rfind(kEndGroup, 0) == 0) {
            if (!stack.empty() && stack.back() == kNone) {   // closes the innermost placeholder group
                stack.pop_back();
                continue;
            }
            // Close up to the matching group; an end marker nobody opened closes nothing.
            const std::string id = uri.substr(kEndGroup.size());
            for (size_t k = stack.size(); k-- > 0;)
                if (stack[k] != kNone && tree.folders[stack[k]].id == id) {
                    stack.resize(k);
                    break;
                }
        } else if (uri.rfind(kPlaylist, 0) == 0) {
            if (!tree.folderOf.emplace(uri, innermost()).second) continue;   // listed twice: the first place wins
            for (size_t s : stack)
                if (s != kNone) ++tree.folders[s].playlistCount;
        }
    }
    std::erase_if(tree.folderOf, [](const auto& e) { return e.second.empty(); });
    return tree;
}

std::vector<TreeRow> treeRows(const std::vector<catalog::Playlist>& playlists, const PlaylistTree& tree,
                              const std::function<bool(const std::string& folderId)>& expanded, const std::string& root) {
    // Folder index by id.
    std::unordered_map<std::string, size_t> folderIndex;
    for (size_t i = 0; i < tree.folders.size(); ++i) folderIndex.emplace(tree.folders[i].id, i);
    auto parentKey = [&](const std::string& folderId) {
        return folderIndex.contains(folderId) ? folderId : std::string();
    };

    struct Child {
        bool folder;
        size_t index;
    };
    std::unordered_map<std::string, std::vector<Child>> children;   // folder id ("" = top) -> children
    for (size_t i = 0; i < playlists.size(); ++i) {
        const auto f = tree.folderOf.find(playlists[i].id);
        children[f == tree.folderOf.end() ? std::string() : parentKey(f->second)].push_back({false, i});
    }
    for (size_t i = 0; i < tree.folders.size(); ++i) {
        std::string parent = parentKey(tree.folders[i].parentId);
        if (parent == tree.folders[i].id) parent.clear();   // a folder can't contain itself
        children[parent].push_back({true, i});
    }

    // Sort key: a playlist's position; a folder's is the smallest key inside it, empty folders after every
    // playlist (in rootlist order). Memoized; a cycle (malformed parents) is cut by the in-progress mark.
    const int64_t kEmpty = static_cast<int64_t>(playlists.size());
    std::vector<int64_t> folderKey(tree.folders.size(), -1);
    std::vector<char> visiting(tree.folders.size(), 0);
    std::function<int64_t(size_t)> keyOf = [&](size_t fi) -> int64_t {
        if (folderKey[fi] >= 0) return folderKey[fi];
        if (visiting[fi]) return INT64_MAX;
        visiting[fi] = 1;
        int64_t best = INT64_MAX;
        if (auto it = children.find(tree.folders[fi].id); it != children.end())
            for (const auto& c : it->second) best = std::min(best, c.folder ? keyOf(c.index) : static_cast<int64_t>(c.index));
        visiting[fi] = 0;
        folderKey[fi] = best < kEmpty ? best : kEmpty + static_cast<int64_t>(fi);
        return folderKey[fi];
    };
    for (size_t fi = 0; fi < tree.folders.size(); ++fi) keyOf(fi);   // all keys before any list is reordered
    for (auto& [_, list] : children)
        std::stable_sort(list.begin(), list.end(), [&](const Child& a, const Child& b) {
            const int64_t ka = a.folder ? folderKey[a.index] : static_cast<int64_t>(a.index);
            const int64_t kb = b.folder ? folderKey[b.index] : static_cast<int64_t>(b.index);
            return ka < kb;
        });

    std::vector<TreeRow> rows;
    std::vector<char> emitted(tree.folders.size(), 0);   // guards against malformed cycles
    std::function<void(const std::string&, int)> emit = [&](const std::string& id, int depth) {
        const auto it = children.find(id);
        if (it == children.end()) return;
        for (const auto& c : it->second) {
            if (!c.folder) {
                rows.push_back({false, depth, c.index});
                continue;
            }
            if (emitted[c.index]) continue;
            emitted[c.index] = 1;
            rows.push_back({true, depth, c.index});
            if (expanded && expanded(tree.folders[c.index].id)) emit(tree.folders[c.index].id, depth + 1);
        }
    };
    if (root.empty() || folderIndex.contains(root)) emit(root, 0);   // an unknown folder has no rows
    return rows;
}

} // namespace st::spotify
