#pragma once
// The tempo (BPM) of songs known on this PC: what the files' own tags say (ID3 TBPM, MP4 tmpo, Vorbis BPM) and what
// ShadeTube's analysis (audio/Tempo) measured on audio it already has (local files, downloads, the song cache). Kept in
// %LOCALAPPDATA%\ShadeTube\bpm.json, per PC (a song's tempo is the same for every account). The app side (app/Bpm)
// fills it on a background worker and answers the track tables, the rule lists and Now Playing.
//
// Keys, like the app's other per-track data: the track ids an entry belongs to (a local file's "local:<hash>", a
// download's catalog id, the catalog ids a cached stream played for and its video as "yt:<videoId>") and the song's
// identity (folded title + first artist, as ListenStats::keysFor), so the same song from another source shows the same
// tempo. A tag beats an analysis for the same id; otherwise the newer entry wins. An analysis that found no steady beat
// is kept too (bpm 0), so the file is not analysed again until it changes (`stamp`).
//
// bpm.json: {"v":1,"e":[{"b":128.0,"s":"t"|"a","c":0.62,"t":<unix>,"f":"<stamp hex>","i":[ids],"k":"<key>"}]}
//
// Standalone: core + nlohmann (tests compile it directly). Not thread-safe (the UI thread owns the app's store).
#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace st::app::bpm {

enum class Source : uint8_t { Tag, Analysis };

struct Entry {
    float bpm = 0;                   // 0 = analysed, no steady beat
    Source source = Source::Analysis;
    float confidence = 0;            // the analysis' (0..1); tags: 1
    int64_t at = 0;                  // unix seconds
    uint64_t stamp = 0;              // the file's size and last write time mixed (0 = unknown): another one = changed
    std::vector<std::string> ids;
    std::wstring key;                // "" = none
};

class Store {
public:
    // Records `e`; its ids and key move to it from older entries (except where an older tag holds them against an
    // analysis). False when nothing changed (the same values for the same keys).
    bool put(Entry e);
    const Entry* byId(const std::string& id) const;
    const Entry* byKey(const std::wstring& key) const;
    // A track's tempo: by its id, its video ("yt:<videoId>"), then its song key. 0 = unknown (or no beat).
    float of(const std::string& id, const std::string& videoId, const std::wstring& key) const;
    // An entry for this id exists and was made from the file with this stamp (0 = any): nothing to do for it.
    bool current(const std::string& id, uint64_t stamp) const;
    // Forgets the analysed values (tags stay): everything is analysed again.
    void clearAnalysis();
    // Forgets what is known for these ids (a tag removed: the file is looked at again).
    bool forget(const std::vector<std::string>& ids);

    size_t size() const { return byId_.size(); }   // ids with an entry
    size_t known() const;                           // ids with a tempo
    size_t known(Source from) const;                // ... from tags / from the analysis
    uint64_t revision() const { return rev_; }      // bumped by every change

    nlohmann::json toJson() const;
    static Store fromJson(const nlohmann::json& j);
    static Store load(const std::filesystem::path& file);    // missing / broken file = empty
    static bool save(const nlohmann::json& j, const std::filesystem::path& file);   // flushed tmp + rename

private:
    void reindex();
    std::vector<Entry> entries_;
    std::unordered_map<std::string, uint32_t> byId_;
    std::unordered_map<std::wstring, uint32_t> byKey_;
    uint64_t rev_ = 0;
};

// A file's stamp from its size and last write time (FILETIME ticks).
uint64_t stampOf(int64_t size, uint64_t mtime);
// "128" for a column / a fact ("" when unknown).
std::wstring label(float bpm);

} // namespace st::app::bpm
