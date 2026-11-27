#pragma once
// The tempo (BPM) of songs in the app: app/BpmStore holds what is known (bpm.json), audio/Tempo measures it. This is
// the app side: the background worker, the lookups for the track tables / rule lists / Now Playing, the settings rows
// and the tag editor's hand-over.
//
// Sources, in order: a BPM tag in the file (ID3 TBPM, MP4 tmpo, Vorbis BPM; read for local files and downloads), else
// ShadeTube's own analysis of audio the app already has: local files, downloads and the song cache (player/AudioCache).
// No catalog offers a tempo without an API key ShadeTube doesn't have, so nothing is asked on the network.
//
// The worker (UI-thread scheduling, one job at a time on a Low priority worker): tags are read in batches of up to 25
// files every few seconds; an analysis decodes 70 s of one song (mono, 11 kHz) at below-normal thread priority and the
// next one waits at least kAnalysisGapMs, so at most ~5 songs a minute; nothing starts while the player is opening a
// song, in the first minute after the start, or with Settings::bpmAnalysis off (tags are still read: no audio is
// decoded). A file is looked at again only when it changed (size / last write time). Results are saved on a worker
// (at most every 30 s) and announced through subscribe(); nothing is shown while it works (Ayarlar › Yerel müzik
// has the counts).
//
// UI thread only (snapshot() is for workers).
#include "app/BpmStore.h"
#include "catalog/Models.h"
#include "core/Async.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::ui {
class Column;
}

namespace st::app::bpm {

inline constexpr int64_t kAnalysisGapMs = 12'000;   // between two analyses (decode + estimate of one song)

void init();   // initFeatures(): loads bpm.json on a worker, then the worker starts its rounds

// A track's tempo (its id, its video, then its song identity); 0 = unknown. Answers are remembered until the store
// changes (a sort of 5 000 rows folds each title once).
float of(const catalog::Track& t);
// The same for a song identity already folded (app/RuleLists keys).
float of(const catalog::Track& t, const std::wstring& key);
// A copy of the store for workers (shared; renewed after changes).
std::shared_ptr<const Store> snapshot();
uint64_t revision();
bool loaded();   // bpm.json has been read
// The store changed (results landed, a tag was written). Coalesced; dies with `owner`.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

// The tag editor wrote a tempo (0 = removed) into the file these ids play from (`stamp`: the file after the write).
void noteTag(const std::vector<std::string>& ids, const std::wstring& key, int bpm, uint64_t stamp);

struct Status {
    size_t known = 0;        // ids with a tempo
    size_t fromTags = 0, fromAnalysis = 0;
    size_t waiting = 0;      // files not looked at yet (as of the last round)
    bool running = false;    // a job is under way
};
Status status();

// Ayarlar › Yerel müzik: the "Tempo analizi" toggle and the counts.
void buildSettingsRows(ui::Column* c, std::function<void()> rebuild);

} // namespace st::app::bpm
