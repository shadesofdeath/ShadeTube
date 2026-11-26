// Video mode's official video: the decision per song (search + alignment on a worker), the "Yanlış video mu?" picker.
// See app/OfficialVideo.h.
#include "app/OfficialVideo.h"

#include "app/AppContext.h"
#include "app/PageWidgets.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/ImageCache.h"
#include "gfx/Theme.h"
#include "ui/Controls.h"
#include "ui/Popups.h"
#include "ui/Window.h"
#include "video/AudioAlign.h"
#include "video/Soundtrack.h"
#include "youtube/MatchService.h"
#include "youtube/OfficialVideo.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace st::app::officialvideo {

using gfx::Color;
using gfx::Rect;
namespace type = gfx::type;

namespace {

// Work on the pool: App destroys the MatchService before it joins the pool, so at exit (shutdown()) every lookup is
// cancelled (its token: the requests; its flag: the downloads and decoding) and awaited before App goes on.
struct InFlight {
    std::mutex m;
    std::condition_variable cv;
    int count = 0;
    YoutubeExplode::CancellationTokenSource exit;
};
InFlight& flight() {
    static InFlight f;
    return f;
}
struct Running {
    Running() {
        std::lock_guard lock(flight().m);
        ++flight().count;
    }
    ~Running() {
        {
            std::lock_guard lock(flight().m);
            --flight().count;
        }
        flight().cv.notify_all();
    }
    Running(const Running&) = delete;
    Running& operator=(const Running&) = delete;
};
bool exiting() { return flight().exit.isCancellationRequested(); }

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

youtube::OfficialQuery queryOf(const catalog::Track& t) {
    youtube::OfficialQuery q;
    q.title = t.name;
    for (const auto& a : t.artists) q.artists.push_back(a.name);
    q.durationSec = static_cast<int>(t.durationMs / 1000);
    q.album = t.album.name;
    return q;
}

// A video's soundtrack as alignment features: its smallest Opus stream, through the engine's built-in decoder (the
// pre-skip trimmed: the presentation timeline the engine's clock and the picture both run on).
video::AudioFeatures soundtrackOf(youtube::MatchService& m, const std::string& videoId, const Song& song,
                                  const std::function<bool()>& cancelled) {
    const auto s = m.soundtrackStream(videoId, "opus", true, flight().exit.token());
    const auto t = video::decodeSoundtrack({s.url, L"", s.contentLength, s.mimeType}, cancelled);
    return video::analyzeAudio(t.mono.data(), t.mono.size(), t.sampleRate);
}

void accept(OfficialDecision& d, const youtube::OfficialCandidate& c, const video::AlignResult& r) {
    d.videoId = c.videoId;
    d.title = c.title;
    d.channel = c.channel;
    d.thumbnailUrl = c.thumbnailUrl;
    d.durationSec = c.durationSec;
    d.segments = r.segments;
    d.score = r.score;
    d.coverage = r.coverage;
    d.verdict = r.verdict;
}

std::string describe(const video::AlignResult& r) {
    std::string s = std::format("{} (coverage {:.0f} %, r {:.2f})", r.verdict, r.coverage * 100, r.score);
    for (const auto& g : r.segments)
        s += std::format("; song {:.2f}-{:.2f} s at {:+.3f} s{}", g.audioStart, g.audioEnd, g.offsetAt(g.audioStart),
                         std::fabs(g.slope - 1) > 1e-6 ? std::format(" x{:.5f}", g.slope) : std::string());
    return s;
}

struct Lookup {
    OfficialDecision decision;
    bool transient = false;   // a candidate could not be fetched: don't store "none"
};

// Worker: candidates, the song's soundtrack, then each candidate's until one aligns. `only`: align just this
// candidate (the user's pick); then the decision is manual.
Lookup search(youtube::MatchService& m, const Song& song, const OfficialDecision* previous, const youtube::OfficialCandidate* only,
              const std::function<bool()>& cancelled) {
    Running running;
    if (exiting()) throw std::runtime_error("cancelled");
    Lookup out;
    OfficialDecision& d = out.decision;
    d.audioVideoId = song.audioVideoId;
    d.decidedAt = nowUnix();
    if (previous && previous->audioVideoId == song.audioVideoId) {
        d.rejected = previous->rejected;
        d.tried = previous->tried;
    }
    std::vector<youtube::OfficialCandidate> candidates;
    if (only) {
        candidates.push_back(*only);
    } else {
        std::vector<std::string> exclude = d.rejected;
        exclude.push_back(song.audioVideoId);
        candidates = youtube::findOfficialVideos(m.client(), queryOf(song.track), exclude, song.region, flight().exit.token());
        ST_LOG_INFO("video", "official video of \"{}\": {} candidate(s){}", song.track.name, candidates.size(),
                    candidates.empty() ? std::string() : ", first " + candidates.front().videoId + " \"" + candidates.front().title + "\"");
    }
    if (candidates.empty()) {
        d.verdict = "no candidates";
        return out;
    }
    const auto t0 = std::chrono::steady_clock::now();
    const video::AudioFeatures songFeatures = soundtrackOf(m, song.audioVideoId, song, cancelled);
    int aligned = 0;
    for (const auto& c : candidates) {
        if (aligned >= 3) break;   // the best three: each costs a soundtrack
        if (cancelled && cancelled()) throw std::runtime_error("cancelled");
        try {
            const auto vf = soundtrackOf(m, c.videoId, song, cancelled);
            ++aligned;
            const auto r = video::alignAudio(songFeatures, vf);
            std::erase_if(d.tried, [&](const auto& t) { return t.first == c.videoId; });
            d.tried.emplace_back(c.videoId, r.accepted ? "match" : r.verdict);
            ST_LOG_INFO("video", "official video {} \"{}\" ({}) against {}: {}", c.videoId, c.title, c.channel, song.audioVideoId, describe(r));
            if (r.accepted) {
                accept(d, c, r);
                d.manual = only != nullptr;
                break;
            }
        } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            if (cancelled && cancelled()) throw;
            ST_LOG_WARN("video", "official video {}: soundtrack failed: {}", c.videoId, e.what());
            out.transient = true;
        }
    }
    if (!d.hasVideo()) d.verdict = aligned ? "not the song's recording" : "failed";
    ST_LOG_INFO("video", "official video of \"{}\": {} in {:.1f} s", song.track.name, d.hasVideo() ? d.videoId : d.verdict,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return out;
}

std::function<bool()> cancelledBy(Lifetime::Ref guard) {
    return [guard] { return guard.expired() || exiting(); };
}

} // namespace

OfficialVideoCache& cache() {
    static OfficialVideoCache* c = [] {
        auto* store = new OfficialVideoCache(paths::cacheDir() / L"officialvideos.json");   // lives for the process
        store->load();
        ctx().persistHooks.push_back([store] { store->flush(); });
        ctx().housekeepingHooks.push_back([store] { store->flush(); });
        return store;
    }();
    return *c;
}

void decide(const Song& songIn, Lifetime::Ref guard, std::function<void(Result<OfficialDecision>)> done) {
    if (auto d = cache().find(songIn.track.id, songIn.audioVideoId, nowUnix())) {
        done(Result<OfficialDecision>::success(std::move(*d)));
        return;
    }
    auto* matcher = ctx().matcher;
    if (!matcher) {
        done(Result<OfficialDecision>::failure(std::make_exception_ptr(std::runtime_error("no matcher"))));
        return;
    }
    Song song = songIn;
    song.region = Settings::get().region;   // Settings belong to the UI thread
    std::optional<OfficialDecision> previous;
    if (const auto* p = cache().raw(song.track.id)) previous = *p;
    async(Priority::Low, guard,
          [matcher, song, previous, guard] {
              return search(*matcher, song, previous ? &*previous : nullptr, nullptr, cancelledBy(guard));
          },
          [song, done](Result<Lookup> r) {
              if (!r) {
                  done(Result<OfficialDecision>::failure(r.error()));
                  return;
              }
              if (!r->transient || r->decision.hasVideo()) cache().put(song.track.id, r->decision);
              done(Result<OfficialDecision>::success(std::move(r->decision)));
          });
}

void reject(const Song& song, std::function<void()> changed) {
    OfficialDecision d;
    if (const auto* p = cache().raw(song.track.id)) d = *p;
    if (d.videoId.empty()) return;
    d.rejected.push_back(d.videoId);
    ST_LOG_INFO("video", "official video {} turned down for \"{}\"", d.videoId, song.track.name);
    // A new search (the next candidate), keeping what was tried and turned down.
    OfficialDecision again;
    again.audioVideoId = song.audioVideoId;
    again.rejected = d.rejected;
    again.tried = d.tried;
    again.version = 0;   // not a usable decision: decide() searches again, with the rejections
    cache().put(song.track.id, again);
    toast(tr(L"Bu video bu şarkı için gösterilmeyecek"));
    if (changed) changed();
}

namespace {

// "Yanlış video mu?" row: thumbnail, title, channel, what the alignment said.
class OfficialRow : public ui::Widget {
public:
    OfficialRow(const youtube::OfficialCandidate& c, std::wstring meta, bool current)
        : current_(current), thumb_(c.thumbnailUrl), title_(toWide(c.title), type::body), channel_(toWide(c.channel), type::caption),
          meta_(std::move(meta), type::monoMeta) {
        focusable = true;
    }
    std::function<void()> onPick;
    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onPick) return false;
        const auto pick = onPick;   // closes the dialog
        pick();
        return true;
    }
    float preferredHeight(float) override { return 72; }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (rect().contains(e.pos) && onPick) {
            const auto pick = onPick;
            pick();
        }
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    void paint(gfx::Canvas& c) override {
        const Rect r = rect();
        const auto& col = gfx::colors();
        if (current_) {
            c.fillRect(r, gfx::accent().tint12);
            c.fillRect({r.x, r.y, 2, r.h}, gfx::accent().base);
        } else {
            c.fillRect(r, col.overlayHover.mulAlpha(hover_));
        }
        const Rect th{r.x + 10, r.cy() - 27, 96, 54};
        if (auto* bmp = gfx::ImageCache::get().request(thumb_, static_cast<int>(96 * c.scale()))) c.image(bmp, th, gfx::radii().art);
        else c.fillRounded(th, gfx::radii().art, col.bgOverlay);
        const float x = th.right() + 14;
        c.text(title_, {x, r.y + 12, r.right() - x - 10, 20}, current_ ? gfx::accent().base : col.fgPrimary, gfx::VAlign::Center);
        c.text(channel_, {x, r.y + 32, r.right() - x - 10, 16}, col.fgSecondary, gfx::VAlign::Center);
        c.text(meta_, {x, r.y + 50, r.right() - x - 10, 14}, col.fgTertiary, gfx::VAlign::Center);
    }

private:
    bool current_;
    std::string thumb_;
    gfx::Text title_, channel_, meta_;
    ui::Anim hover_;
};

std::wstring metaOf(const youtube::OfficialCandidate& c, const OfficialDecision* d) {
    const std::wstring duration = ui::formatDuration(c.durationSec * 1000LL);
    const std::string why = d ? d->verdictOf(c.videoId) : std::string();
    if (d && d->videoId == c.videoId && d->hasVideo()) return i18n::format(tr(L"{} · SESLE EŞLEŞİYOR"), {duration});
    if (d && std::find(d->rejected.begin(), d->rejected.end(), c.videoId) != d->rejected.end())
        return i18n::format(tr(L"{} · GÖSTERİLMİYOR"), {duration});
    if (why == "match") return i18n::format(tr(L"{} · SESLE EŞLEŞİYOR"), {duration});
    if (!why.empty() && why != "failed") return i18n::format(tr(L"{} · AYNI KAYIT DEĞİL"), {duration});
    return i18n::format(c.omv ? tr(L"{} · RESMİ VİDEO") : tr(L"{} · DENENMEDİ"), {duration});
}

// The user's pick: aligned first; shown only when it is the song's recording.
void pick(const Song& songIn, const youtube::OfficialCandidate& c, std::function<void()> changed) {
    auto* matcher = ctx().matcher;
    if (!matcher) return;
    Song song = songIn;
    song.region = Settings::get().region;
    toast(tr(L"Video sesle eşleştiriliyor…"));
    std::optional<OfficialDecision> previous;
    if (const auto* p = cache().raw(song.track.id)) previous = *p;
    auto guard = std::make_shared<Lifetime>();
    static std::vector<std::shared_ptr<Lifetime>> running;   // keeps the guard while the work runs
    running.push_back(guard);
    async(Priority::Normal, guard->ref(),
          [matcher, song, c, previous, ref = guard->ref()] {
              return search(*matcher, song, previous ? &*previous : nullptr, &c, cancelledBy(ref));
          },
          [song, c, changed, guard](Result<Lookup> r) {
              std::erase(running, guard);
              if (!r) {
                  toast(tr(L"Video denetlenemedi: bağlantıyı kontrol et"), true);
                  return;
              }
              if (!r->decision.hasVideo()) {
                  // Remember that it was tried, keep whatever played before.
                  if (const auto* p = cache().raw(song.track.id)) {
                      OfficialDecision keep = *p;
                      keep.tried = r->decision.tried;
                      cache().put(song.track.id, std::move(keep));
                  }
                  toast(r->transient ? tr(L"Video denetlenemedi: bağlantıyı kontrol et") : tr(L"Bu video şarkının kaydıyla eşleşmiyor: gösterilmiyor"),
                        true);
                  return;
              }
              cache().put(song.track.id, r->decision);
              toast(i18n::format(tr(L"Video güncellendi: {}"), {toWide(c.channel.empty() ? c.title : c.channel)}));
              if (changed) changed();
          });
}

} // namespace

void showPicker(const Song& song, std::function<void()> changed) {
    auto* matcher = ctx().matcher;
    if (!matcher || !ctx().window) return;
    auto* d = ui::Dialog::open(ctx().window, tr(L"Yanlış video mu?"),
                               i18n::format(tr(L"\"{}\" için bulunan müzik videoları. Seçtiğin video önce sesle eşleştirilir; şarkının "
                                               L"kaydıyla aynı değilse gösterilmez."),
                                            {toWide(song.track.name)}),
                               640);
    if (!d) return;
    auto* body = static_cast<ui::Column*>(d->body());
    body->add<Spacer>(60.f);
    d->addButton(tr(L"Video gösterme"), ui::ButtonKind::Secondary, [song, changed] {
        OfficialDecision none;
        if (const auto* p = cache().raw(song.track.id)) none = *p;
        none.audioVideoId = song.audioVideoId;
        none.videoId.clear();
        none.segments.clear();
        none.manual = true;
        none.verdict = "turned off by the user";
        none.version = OfficialDecision::kVersion;
        none.decidedAt = nowUnix();
        cache().put(song.track.id, std::move(none));
        toast(tr(L"Bu şarkıda video gösterilmeyecek"));
        if (changed) changed();
    });
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    auto guard = std::make_shared<Lifetime>();
    d->onClosed = [guard] { guard->renew(); };
    const youtube::OfficialQuery q = queryOf(song.track);
    const std::string exclude = song.audioVideoId;
    const std::string region = Settings::get().region;   // Settings belong to the UI thread
    async(Priority::High, guard->ref(),
          [matcher, q, exclude, region] {
              Running running;
              if (exiting()) throw std::runtime_error("cancelled");
              return youtube::findOfficialVideos(matcher->client(), q, {exclude}, region, flight().exit.token());
          },
          [d, body, song, changed](Result<std::vector<youtube::OfficialCandidate>> r) {
              body->clearChildren();
              const auto* decision = cache().raw(song.track.id);
              std::vector<youtube::OfficialCandidate> list = r ? *r : std::vector<youtube::OfficialCandidate>{};
              // The video in use (or turned down) stays listed even when the search ranks it lower today.
              if (decision && !decision->videoId.empty() &&
                  std::none_of(list.begin(), list.end(), [&](const auto& c) { return c.videoId == decision->videoId; })) {
                  youtube::OfficialCandidate cur;
                  cur.videoId = decision->videoId;
                  cur.title = decision->title;
                  cur.channel = decision->channel;
                  cur.thumbnailUrl = decision->thumbnailUrl;
                  cur.durationSec = decision->durationSec;
                  list.insert(list.begin(), cur);
              }
              if (list.empty()) {
                  body->add<ui::Label>(r ? tr(L"Bu şarkının resmi müzik videosu bulunamadı.") : tr(L"Videolar aranamadı: bağlantıyı kontrol et."),
                                       type::bodyRegular, ui::Tone::Secondary);
                  d->requestLayout();
                  return;
              }
              for (const auto& c : list) {
                  const bool current = decision && decision->videoId == c.videoId && decision->hasVideo();
                  auto* row = body->add<OfficialRow>(c, metaOf(c, decision), current);
                  row->onPick = [d, song, c, changed, current] {
                      d->close();
                      if (!current) pick(song, c, changed);
                  };
              }
              d->requestLayout();
              if (ctx().window) ctx().window->invalidate();
          });
}

void shutdown() {
    flight().exit.cancel();
    std::unique_lock lock(flight().m);
    if (!flight().cv.wait_for(lock, std::chrono::seconds(10), [] { return flight().count == 0; }))
        ST_LOG_WARN("video", "official video: {} lookup(s) still running at exit", flight().count);
}

} // namespace st::app::officialvideo
