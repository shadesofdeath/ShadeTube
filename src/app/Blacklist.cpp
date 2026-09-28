// Kara liste (see Blacklist.h). The two lists are tiny (tens of entries): every change rebuilds the lookup indexes
// and rewrites blacklist.json (write, check, then an atomic replace), so a failed write never replaces a good file.
// A damaged file (not JSON, wrong shapes) is kept as blacklist.json.bad before anything can overwrite it.
#include "app/Blacklist.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <windows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace st::app::blacklist {

namespace {

using json = nlohmann::json;
using catalog::ArtistRef;
using catalog::Track;

// Title + first artist of another release counts as the same recording within this duration difference.
constexpr int kSameRecordingMs = 3000;

struct KeyEntry {
    int durationMs = 0;
    std::string catalog;
};

struct State {
    std::filesystem::path file;
    bool loaded = false;
    std::vector<Entry> tracks, artists;
    // Indexes (rebuilt from the lists).
    std::unordered_set<std::string> trackIds, artistIds;
    std::unordered_map<std::string, std::vector<KeyEntry>> trackKeys;        // title + first artist -> entries
    std::unordered_map<std::string, std::vector<std::string>> artistNames;   // folded name -> catalogs of its entries
    std::vector<std::function<void()>> listeners;
    uint64_t revision = 1;
};

State& state() {
    static State s;
    return s;
}

int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string fold(const std::string& s) {
    std::wstring w = foldForSearch(toWide(s));
    const auto ws = [](wchar_t c) { return c == L' ' || c == L'\t'; };
    while (!w.empty() && ws(w.back())) w.pop_back();
    size_t i = 0;
    while (i < w.size() && ws(w[i])) ++i;
    return toUtf8(std::wstring_view(w).substr(i));
}

// "spotify", "mb" (a bare MBID), the prefix of other ids ("local", "import", "preview"...), "" for no id.
std::string catalogOf(const std::string& id) {
    if (id.empty()) return {};
    if (id.rfind("spotify:", 0) == 0) return "spotify";
    const size_t colon = id.find(':');
    return colon == std::string::npos ? std::string("mb") : id.substr(0, colon);
}

std::string trackKey(const std::string& name, const std::string& artist0) {
    if (name.empty() || artist0.empty()) return {};
    const std::string n = fold(name), a = fold(artist0);
    return n.empty() || a.empty() ? std::string{} : n + '\x1f' + a;
}

std::string trackKey(const Track& t) { return trackKey(t.name, t.artists.empty() ? std::string{} : t.artists[0].name); }

// The title + first artist fallback is for another release of the same recording, not for a different song that
// shares a generic title ("Intro", "I. Allegro"): both durations known -> they must agree within kSameRecordingMs; a
// duration unknown (local files, MusicBrainz, entries without one) -> only across catalogs.
bool sameRecording(int durationA, const std::string& catalogA, int durationB, const std::string& catalogB) {
    if (durationA > 0 && durationB > 0) return std::abs(durationA - durationB) <= kSameRecordingMs;
    return catalogA != catalogB;
}

void reindex() {
    auto& s = state();
    s.trackIds.clear();
    s.trackKeys.clear();
    s.artistIds.clear();
    s.artistNames.clear();
    for (const auto& e : s.tracks) {
        if (!e.id.empty()) s.trackIds.insert(e.id);
        if (auto k = trackKey(e.name, e.artist0); !k.empty()) s.trackKeys[std::move(k)].push_back({e.durationMs, catalogOf(e.id)});
    }
    for (const auto& e : s.artists) {
        if (!e.id.empty()) s.artistIds.insert(e.id);
        if (!e.name.empty()) s.artistNames[fold(e.name)].push_back(catalogOf(e.id));
    }
}

std::filesystem::path storeFile() {
    auto& s = state();
    if (s.file.empty()) s.file = paths::appData() / L"blacklist.json";
    return s.file;
}

// Type-checked readers: a hand-edited field of the wrong type reads as empty / 0 instead of throwing.
std::string str(const json& o, const char* key) {
    const auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

int64_t num(const json& o, const char* key) {
    const auto it = o.find(key);
    if (it == o.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    return 0;
}

// Keeps a damaged file as blacklist.json.bad, so the next save (which writes only what could be read) loses nothing.
void keepAside() {
    std::error_code ec;
    auto bad = storeFile();
    bad += L".bad";
    std::filesystem::copy_file(storeFile(), bad, std::filesystem::copy_options::overwrite_existing, ec);
    ST_LOG_WARN("blacklist", "blacklist.json is damaged: kept as blacklist.json.bad{}", ec ? " (copy failed)" : "");
}

void readFile() {
    auto& s = state();
    s.tracks.clear();
    s.artists.clear();
    bool damaged = false;
    try {
        std::ifstream f(storeFile(), std::ios::binary);
        if (f) {
            const json j = json::parse(f, nullptr, false);
            f.close();
            if (!j.is_object()) {
                damaged = true;
            } else {
                auto readList = [&](const char* key, bool tracks) {
                    const auto it = j.find(key);
                    if (it == j.end()) return;
                    if (!it->is_array()) {
                        damaged = true;
                        return;
                    }
                    for (const auto& o : *it) {
                        try {
                            if (!o.is_object()) {
                                damaged = true;
                                continue;
                            }
                            Entry e{str(o, "id"), str(o, "n"), tracks ? str(o, "a") : std::string{}, tracks ? str(o, "a0") : std::string{},
                                    num(o, "t"), tracks ? static_cast<int>(num(o, "d")) : 0};
                            if (e.id.empty() && e.name.empty()) {
                                damaged = true;
                                continue;
                            }
                            (tracks ? s.tracks : s.artists).push_back(std::move(e));
                        } catch (const std::exception&) {
                            damaged = true;
                        }
                    }
                };
                readList("tracks", true);
                readList("artists", false);
            }
        }
    } catch (const std::exception& e) {   // never let this file stop the app from starting
        ST_LOG_WARN("blacklist", "blacklist.json unreadable: {}", e.what());
        damaged = true;
    }
    if (damaged) keepAside();
    reindex();
    s.loaded = true;
    if (!s.tracks.empty() || !s.artists.empty())
        ST_LOG_INFO("blacklist", "loaded: {} tracks, {} artists", s.tracks.size(), s.artists.size());
}

void save() {
    const auto& s = state();
    json tracks = json::array(), artists = json::array();
    for (const auto& e : s.tracks)
        tracks.push_back({{"id", e.id}, {"n", e.name}, {"a", e.artists}, {"a0", e.artist0}, {"t", e.addedAt}, {"d", e.durationMs}});
    for (const auto& e : s.artists) artists.push_back({{"id", e.id}, {"n", e.name}, {"t", e.addedAt}});
    const json j = {{"version", 1}, {"tracks", std::move(tracks)}, {"artists", std::move(artists)}};
    const std::string data = j.dump(-1, ' ', false, json::error_handler_t::replace);   // never throws on bad UTF-8
    auto tmp = storeFile();
    tmp += L".tmp";
    std::error_code ec;
    {
        std::ofstream f(tmp, std::ios::trunc | std::ios::binary);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        f.close();   // flushes: a full disk shows up here, before the good file is replaced
        if (f.fail()) {
            ST_LOG_ERROR("blacklist", "failed to write blacklist.json");
            std::filesystem::remove(tmp, ec);
            return;
        }
    }
    if (!MoveFileExW(tmp.c_str(), storeFile().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ST_LOG_ERROR("blacklist", "failed to replace blacklist.json (error {})", GetLastError());
        std::filesystem::remove(tmp, ec);
    }
}

void changed() {
    ++state().revision;
    reindex();
    save();
    auto ls = state().listeners;   // a listener may register another one
    for (auto& fn : ls)
        if (fn) fn();
}

void ensureLoaded() {
    if (!state().loaded) readFile();
}

bool artistEntryMatches(const Entry& e, const ArtistRef& a) {
    if (!a.id.empty() && e.id == a.id) return true;
    if (a.name.empty() || e.name.empty() || fold(e.name) != fold(a.name)) return false;
    return a.id.empty() || e.id.empty() || catalogOf(e.id) != catalogOf(a.id);
}

bool trackEntryMatches(const Entry& e, const Track& t) {
    if (!t.id.empty() && e.id == t.id) return true;
    const std::string k = trackKey(t);
    return !k.empty() && k == trackKey(e.name, e.artist0) && sameRecording(e.durationMs, catalogOf(e.id), t.durationMs, catalogOf(t.id));
}

} // namespace

bool trackMatches(const Track& t) {
    ensureLoaded();
    const auto& s = state();
    if (s.tracks.empty()) return false;
    if (!t.id.empty() && s.trackIds.contains(t.id)) return true;
    const std::string k = trackKey(t);
    if (k.empty()) return false;
    const auto it = s.trackKeys.find(k);
    if (it == s.trackKeys.end()) return false;
    const std::string mine = catalogOf(t.id);
    return std::any_of(it->second.begin(), it->second.end(),
                       [&](const KeyEntry& e) { return sameRecording(e.durationMs, e.catalog, t.durationMs, mine); });
}

bool artistMatches(const ArtistRef& a) {
    ensureLoaded();
    const auto& s = state();
    if (s.artists.empty()) return false;
    if (!a.id.empty() && s.artistIds.contains(a.id)) return true;
    if (a.name.empty()) return false;
    const auto it = s.artistNames.find(fold(a.name));
    if (it == s.artistNames.end()) return false;
    if (a.id.empty()) return true;
    const std::string mine = catalogOf(a.id);
    return std::any_of(it->second.begin(), it->second.end(), [&](const std::string& c) { return c.empty() || c != mine; });
}

bool isBlocked(const Track& t) {
    ensureLoaded();
    if (empty()) return false;
    if (trackMatches(t)) return true;
    return std::any_of(t.artists.begin(), t.artists.end(), [](const ArtistRef& a) { return artistMatches(a); });
}

bool isTrackBlocked(const std::string& trackId) {
    ensureLoaded();
    return !trackId.empty() && state().trackIds.contains(trackId);
}

bool isArtistBlocked(const std::string& artistId) {
    ensureLoaded();
    return !artistId.empty() && state().artistIds.contains(artistId);
}

void setTrackBlocked(const Track& t, bool blocked) {
    ensureLoaded();
    auto& s = state();
    if (blocked) {
        if (trackMatches(t) || (t.id.empty() && t.name.empty())) return;
        Entry e{t.id, t.name, t.artistLine(), t.artists.empty() ? std::string{} : t.artists[0].name, nowSeconds(), t.durationMs};
        s.tracks.insert(s.tracks.begin(), std::move(e));
        ST_LOG_INFO("blacklist", "track blocked: {}", t.name);
    } else {
        // Every entry that blocks this track (its id, or another release of the same recording).
        const size_t before = s.tracks.size();
        std::erase_if(s.tracks, [&](const Entry& e) { return trackEntryMatches(e, t); });
        if (s.tracks.size() == before) return;
        ST_LOG_INFO("blacklist", "track unblocked: {}", t.name);
    }
    changed();
}

void setArtistBlocked(const std::string& artistId, const std::string& artistName, bool blocked) {
    ensureLoaded();
    auto& s = state();
    const ArtistRef ref{artistId, artistName};
    if (blocked) {
        if ((artistId.empty() && artistName.empty()) || artistMatches(ref)) return;
        s.artists.insert(s.artists.begin(), Entry{artistId, artistName, {}, {}, nowSeconds(), 0});
        ST_LOG_INFO("blacklist", "artist blocked: {}", artistName);
    } else {
        const size_t before = s.artists.size();
        std::erase_if(s.artists, [&](const Entry& e) { return artistEntryMatches(e, ref); });
        if (s.artists.size() == before) return;
        ST_LOG_INFO("blacklist", "artist unblocked: {}", artistName);
    }
    changed();
}

void remove(const Entry& entry) {
    ensureLoaded();
    auto& s = state();
    const auto same = [&](const Entry& e) { return e.id == entry.id && e.name == entry.name && e.addedAt == entry.addedAt; };
    const size_t before = s.tracks.size() + s.artists.size();
    std::erase_if(s.tracks, same);
    std::erase_if(s.artists, same);
    if (s.tracks.size() + s.artists.size() == before) return;
    ST_LOG_INFO("blacklist", "entry removed: {}", entry.name);
    changed();
}

const std::vector<Entry>& blockedTracks() {
    ensureLoaded();
    return state().tracks;
}

const std::vector<Entry>& blockedArtists() {
    ensureLoaded();
    return state().artists;
}

bool empty() {
    ensureLoaded();
    return state().tracks.empty() && state().artists.empty();
}

uint64_t revision() { return state().revision; }

void load() { ensureLoaded(); }

void setStoreFile(const std::filesystem::path& file) {
    state().file = file;
    ++state().revision;
    readFile();
}

void onChanged(std::function<void()> fn) {
    if (fn) state().listeners.push_back(std::move(fn));
}

} // namespace st::app::blacklist
