#pragma once
// Song preview: the state machine behind a press and hold on a row / card, the "Önizle" key and the track menu's
// "Önizle". Standalone (catalog models only; tests/preview compiles it directly): the player, the catalog lookups and
// the clock come in through Port and explicit times. app/Preview.cpp is the app side (input, overlay, settings).
//
//   begin()      a hold took the press (Trigger::Hold), the menu item or the key: a song starts resolving at once; a
//                collection (album / playlist / artist / Liked Songs) first looks up its song (cached for the run)
//   audioStarted the player's preview became audible: Playing, the 20 s limit runs from here
//   release()    the held button went up: a hold's preview stops (before its audio: cancelled, nothing was heard)
//   escape()     Esc stops any preview; toggle() (the key) starts one or stops the running one
//   tick()       the limit, a resolve that takes too long, a hint's time
// A target that is the song already playing shows a hint ("Şu an çalıyor") instead; so do a failed lookup / stream.
// A new begin() stops the previous preview first. Every Port::start() is matched by exactly one Port::stop() unless
// the player ended or failed it itself (ended() / failed()).
#include "catalog/Models.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace st::app::preview {

// Settings::previewMode: "hold" (press and hold + the menu + the key), "menu" (the menu item and the key only), "off".
enum class Mode { Hold, Menu, Off };
Mode parseMode(std::string_view text);   // unknown text = Hold
const char* modeName(Mode m);

inline constexpr double kHoldMs = 350;              // a press rests this long before it previews
inline constexpr double kLimitMs = 20'000;          // the longest preview, from its first sound
inline constexpr double kResolveTimeoutMs = 12'000; // no sound by then: given up ("Önizleme açılamadı")
inline constexpr double kHintMs = 1'800;            // a hint of a menu / key preview stays this long (a hold's until release)

// Where a preview of a song of `durationMs` starts: about 35 % in (a representative part, past the intro), early enough
// for the whole preview to fit, moved past a skipped (SponsorBlock: non-music) segment it falls into; the top for a
// short song or an unknown length. `skip`: [start, end) ms, any order.
int64_t startPoint(int64_t durationMs, const std::vector<std::pair<int64_t, int64_t>>& skip = {});

// A collection whose first / most popular song a card previews.
struct Collection {
    enum class Kind { None, Album, Playlist, Artist, Liked };
    Kind kind = Kind::None;
    std::string id;            // album / playlist / artist id (Liked: empty)
    std::string key() const;   // "album:<id>", ... (the lookup cache)
};

struct Target {
    std::optional<catalog::Track> track;   // a song, or
    Collection collection;                 // a collection (track empty)
    std::wstring title, subtitle;          // shown until the song is known (a collection's name)
    std::vector<catalog::Image> images;
};
Target songTarget(const catalog::Track& t);

enum class Trigger { Hold, Menu, Key };
enum class Phase { Idle, Looking, Resolving, Playing, Hint };
enum class Hint { None, AlreadyPlaying, NoSong, Failed };

class Port {
public:
    virtual ~Port() = default;
    virtual std::string currentTrackId() const = 0;                 // the item that plays ("" = none)
    virtual bool canPreview(const catalog::Track& t) const = 0;     // a playable song (no station / episode)
    // Finds the collection's song, later: Session::found / notFound with this id. false = can't look it up at all.
    virtual bool lookup(uint64_t id, const Collection& c) = 0;
    // Starts the player's preview, later: audioStarted / failed / ended with this id. false = refused.
    virtual bool start(uint64_t id, const catalog::Track& song) = 0;
    virtual void stop(uint64_t id) = 0;
};

class Session {
public:
    explicit Session(Port& port) : port_(port) {}
    Mode mode = Mode::Hold;

    // false: nothing to preview here (mode, not a song, a hold while the mode is "menu"): the press stays a press.
    bool begin(Target target, Trigger trigger, double nowMs);
    // The key: stops a running preview, else begin(..., Trigger::Key).
    bool toggle(Target target, double nowMs);
    void release(double nowMs);
    bool escape();          // true when it stopped something
    void tick(double nowMs);

    // From the Port.
    void found(uint64_t id, const catalog::Track& song, double nowMs);
    void notFound(uint64_t id, double nowMs);
    void audioStarted(uint64_t id, double nowMs);
    void failed(uint64_t id, double nowMs);
    void ended(uint64_t id);   // the player stopped it itself (its end, a playback command)

    Phase phase() const { return phase_; }
    bool active() const { return phase_ != Phase::Idle; }
    Hint hint() const { return hint_; }
    Trigger trigger() const { return trigger_; }
    bool held() const { return held_; }
    uint64_t id() const { return id_; }
    const Target& target() const { return target_; }
    const std::optional<catalog::Track>& song() const { return song_; }   // known once looked up
    float progress(double nowMs) const;   // 0..1 of the limit while Playing
    double phaseSince() const { return phaseAt_; }

private:
    void startSong(const catalog::Track& song, double nowMs);
    void showHint(Hint h, double nowMs);
    void setPhase(Phase p, double nowMs);
    void stopActive();

    Port& port_;
    Phase phase_ = Phase::Idle;
    Hint hint_ = Hint::None;
    Trigger trigger_ = Trigger::Hold;
    uint64_t id_ = 0;
    Target target_;
    std::optional<catalog::Track> song_;
    double began_ = 0, phaseAt_ = 0;
    bool held_ = false;      // a hold whose button is still down
    bool started_ = false;   // Port::start() ran for id_ and no stop / end / failure followed yet
    bool looking_ = false;
    std::vector<std::pair<std::string, catalog::Track>> cache_;   // collection key -> its song (newest last)
};

} // namespace st::app::preview
