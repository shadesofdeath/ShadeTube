#include "app/LyricsRomanization.h"

#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"

namespace st::app {

bool LyricsRomanization::on() const { return Settings::get().lyricsRomanize; }

const std::string& LyricsRomanization::line(size_t i) const {
    static const std::string none;
    return state_ == State::Ready && i < result_.lines.size() ? result_.lines[i] : none;
}

void LyricsRomanization::changed() {
    if (onChange) onChange();
}

void LyricsRomanization::clear() {
    life_.renew();
    lyrics_.reset();
    key_.clear();
    result_ = {};
    state_ = State::None;
    on_ = on();
}

void LyricsRomanization::setLyrics(const lyrics::Lyrics& l, const std::string& trackKey) {
    life_.renew();
    result_ = {};
    lyrics_ = std::make_shared<const lyrics::Lyrics>(l);   // the workers' copy
    key_ = trackKey;
    on_ = on();
    if (l.instrumental || l.lines.empty()) {
        state_ = State::None;
        changed();
        return;
    }
    // Which script (and a cached answer, shown at once) - off the UI thread like everything that reads the text.
    state_ = State::Detecting;
    struct Found {
        lyrics::Script script = lyrics::Script::None;
        std::optional<lyrics::Romanization> ready;
    };
    async(Priority::Normal, life_.ref(),
          [l = lyrics_, key = key_] {
              Found f;
              f.script = lyrics::romanizableScript(*l);
              if (f.script != lyrics::Script::None) f.ready = lyrics::cachedRomanization(*l, key);
              return f;
          },
          [this](Result<Found> r) {
              state_ = r && r->script != lyrics::Script::None ? State::Offered : State::None;
              if (state_ == State::Offered && r->ready) {
                  result_ = std::move(*r->ready);
                  state_ = State::Ready;
              }
              if (state_ == State::Offered && on()) start();
              else changed();
          });
}

void LyricsRomanization::start() {
    if (!lyrics_) return;
    state_ = State::Loading;
    changed();
    async(Priority::Normal, life_.ref(),
          [l = lyrics_, key = key_] {
              // Kanji readings from Windows' analyzer when the lyrics are Japanese (its own STA thread, for this job).
              std::unique_ptr<lyrics::JapaneseReader> reader;
              if (lyrics::romanizableScript(*l) == lyrics::Script::Japanese) reader = lyrics::systemJapaneseReader();
              return lyrics::romanize(*l, key, reader.get());
          },
          [this](Result<std::optional<lyrics::Romanization>> r) {
              if (!r || !*r) {
                  state_ = State::Failed;
              } else if ((*r)->script == lyrics::Script::None) {
                  state_ = State::None;
              } else {
                  result_ = std::move(**r);
                  state_ = State::Ready;
              }
              changed();
          });
}

void LyricsRomanization::sync() {
    if (!lyrics_) return;
    const bool o = on();
    if (o == on_) return;
    on_ = o;
    if (o && (state_ == State::Offered || state_ == State::Failed)) {
        start();
        return;
    }
    if (!o && state_ == State::Failed) state_ = State::Offered;
    changed();
}

void LyricsRomanization::toggle() {
    auto& s = Settings::get();
    s.lyricsRomanize = !s.lyricsRomanize;
    s.markDirty();
    sync();
}

std::wstring LyricsRomanization::note() const {
    if (!on()) return {};
    switch (state_) {
    case State::Loading: return toUpperTr(tr(L"Romanizasyon hazırlanıyor…"));
    case State::Failed: return toUpperTr(tr(L"Romanizasyon yapılamadı"));
    case State::Ready: {
        std::wstring s = toUpperTr(tr(L"Romanizasyon")) + L" · ";
        switch (result_.script) {
        case lyrics::Script::Japanese: s += toUpperTr(tr(L"Japonca")); break;
        case lyrics::Script::Korean: s += toUpperTr(tr(L"Korece")); break;
        default: s += toUpperTr(result_.language == "uk" ? tr(L"Ukraynaca") : tr(L"Rusça")); break;
        }
        if (result_.script == lyrics::Script::Japanese && !result_.readings) s += L" · " + toUpperTr(tr(L"Kanji okunuşu yok"));
        return s;
    }
    default: return {};
    }
}

} // namespace st::app
