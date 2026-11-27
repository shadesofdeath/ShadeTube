#pragma once
// A lyrics view's romanization ("Romanizasyon", lyrics/Romanize): the Latin-letter reading of Japanese, Korean or
// Cyrillic lyrics under each line, in the same family as the translation (LyricsTranslation): offered only when the
// lyrics are in one of those scripts (found out off the UI thread), switched by Settings::lyricsRomanize (shared by Now
// Playing and the full-screen view), worked out off the UI thread and cached on disk per track.
//
// Shown under the original line, above its translation, rather than instead of it: the karaoke fill follows the
// original's word timings, a reading without kanji readings (no Japanese language data on the PC) still needs the
// original next to it, and readers of the script keep it. UI thread.
#include "lyrics/Lyrics.h"
#include "lyrics/Romanize.h"

#include "core/Async.h"

#include <functional>
#include <memory>
#include <string>

namespace st::app {

class LyricsRomanization {
public:
    enum class State {
        None,        // nothing to offer: no lyrics, or nothing in a script it reads
        Detecting,   // finding out the script
        Offered,     // Japanese / Korean / Cyrillic; not worked out yet
        Loading,     // working it out
        Ready,       // shown while the setting is on
        Failed,
    };
    std::function<void()> onChange;   // the state or the setting changed: relayout / repaint

    void setLyrics(const lyrics::Lyrics& l, const std::string& trackKey);
    void clear();
    void sync();                      // catches up with the setting changed in the other view (cheap; called as they paint)
    void toggle();                    // the "Romanizasyon" button: flips Settings::lyricsRomanize (both views)

    State state() const { return state_; }
    bool offered() const { return state_ >= State::Offered; }
    bool on() const;                  // Settings::lyricsRomanize
    bool shown() const { return on() && state_ == State::Ready; }
    const std::string& line(size_t i) const;   // "" = nothing under line i
    // The header's note while it is on: "ROMANİZASYON · KORECE", "... · JAPONCA · KANJİ OKUNUŞU YOK", "" = none.
    std::wstring note() const;

private:
    void start();
    void changed();
    Lifetime life_;
    std::shared_ptr<const lyrics::Lyrics> lyrics_;
    std::string key_;
    bool on_ = false;
    State state_ = State::None;
    lyrics::Romanization result_;
};

} // namespace st::app
