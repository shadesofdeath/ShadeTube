#pragma once
// Video mode's official video (app/OfficialVideo): where the song's audio is in a music video's own soundtrack, so the
// picture of an official video can play against an Art Track / audio upload. Pure maths over PCM (tests/video feeds it
// synthetic signals); app/OfficialVideo fetches both soundtracks (video/Soundtrack) on a worker thread.
//
// Features (analyzeAudio): a 10 ms grid (100 frames/s) of onset strength in 5 bands (log band energy, its rise from
// the previous frame), from a 512-point STFT at 12 kHz. Log energies are relative to the signal's median, so a louder
// or quieter master of the same mix gives the same features; onsets make the correlation peak sharp (to ~10 ms).
//
// Alignment (alignAudio): the song is cut into 8 s windows every 2 s. Each window is correlated (Pearson, summed over
// the bands) against every position of the video's soundtrack, first on a 40 ms grid (FFT), then its best few peaks
// again at 10 ms with a parabolic peak fit: the lags at which the window may sit in the video. A dynamic programme
// over the windows picks one lag (or "no match") per window, maximizing the correlation while charging every change of
// lag: repeated identical choruses (whose windows correlate as well with the other chorus) take their neighbours' lag.
// Runs of windows on one lag become segments: a line video = videoStart + slope * (audio - audioStart) fitted to their
// lags (a constant speed difference of a remaster / transfer, under 0.6 %, is followed; a run whose lags leave its
// line by more than 25 ms is split, and one that can't be split into such lines is drift and dropped; lags are first
// propagated to the neighbouring windows, so a run can go on through a window whose own best peaks missed it). Runs on
// one line separated by a quiet or masked part are one
// segment (the video's timeline goes on through it). Segment ends are found to a frame by the local correlation (1.5 s)
// along each line; between two segments the change point is where the next line fits better. The first and last
// segment also reach over the song's quiet start and end.
//
// Bands are weighted equally (each band's onset strength at unit deviation): a regular kick lines up at many lags, the
// upper bands say where in the song a window is. A run is kept when its windows fit with r >= 0.5 on average and, if
// shorter than 30 s, stand out from their next best lag by 0.08.
//
// Verdict: the match is used when the segments cover at least 65 % of the song's sounding part, at most 3 holds
// interrupt it (gaps, jumps over 0.25 s; smaller steps are micro-edits that play through), the chosen lags lead each
// window's next best by 0.05 on average (two different songs on one beat grid don't) and the segments follow each
// other in the video (an edit that reorders the song is refused). Otherwise "none": the cover shows as for any song
// without a video. Uncovered parts of an accepted match are held (TimeMap): the picture fades out and waits on the
// next segment's first frame.
//
// Measured 2026-11-26 on real songs (Art Track's Opus soundtrack against the official video's): offsets +9.43 /
// +14.08 / +51.61 s (intros), a 0.25 % slower transfer followed as a slope, an edit with a 2.6 s insert and 20-40 ms
// micro-edits; against the waveforms (sample-accurate cross-correlation) mean errors 0.2-4 ms, worst 15 ms (30 ms in
// a micro-edit stretch). A live performance, another song and different songs on one artist declined. ~0.5 s per
// pair of 4 minute soundtracks (Debug build).
#include <cstdint>
#include <string>
#include <vector>

namespace st::video {

struct AudioFeatures {
    static constexpr int kBands = 5;
    static constexpr int kFps = 100;
    int frames = 0;
    std::vector<float> flux[kBands];   // onset strength per band and frame
    std::vector<float> levelDb;        // frame energy against the signal's median (dB): quiet parts
    double seconds() const { return static_cast<double>(frames) / kFps; }
};

// `mono` at `sampleRate` (a multiple of 100; 12000 is what video/Soundtrack delivers).
AudioFeatures analyzeAudio(const float* mono, size_t count, int sampleRate);

// A part of the song that plays against the video at a fixed relation.
struct AlignSegment {
    double audioStart = 0, audioEnd = 0;   // song seconds (the engine's clock)
    double videoStart = 0;                 // video seconds at audioStart
    double slope = 1;                      // video seconds per song second
    double score = 0;                      // mean correlation of its windows
    double videoAt(double audioSec) const { return videoStart + slope * (audioSec - audioStart); }
    double offsetAt(double audioSec) const { return videoAt(audioSec) - audioSec; }
};

struct AlignOptions {
    double windowSec = 8, hopSec = 2;
    double maxLagSec = 300;          // how far the song may sit from the video's start, either way
    double matchR = 0.45;            // a window's correlation worth more than "no match"
    double changePenalty = 1.5;      // the cost of a change of lag between windows
    double maxSlopeDev = 0.006;      // |slope - 1| followed as a speed difference
    double maxResidualSec = 0.025;   // a run's lags may leave its line by this much (else it is split, or drift)
    double minSegmentScore = 0.5;
    double minMargin = 0.05;         // the chosen lags' mean lead over each window's next best lag
    double minCoverage = 0.65;
    double smallJumpSec = 0.25;      // segments next to each other this close in offset: a micro-edit, no hold
    int maxJumps = 3;                // holds (gaps, larger jumps) at most
};

struct AlignResult {
    bool accepted = false;
    std::vector<AlignSegment> segments;   // accepted: in song order, at least one
    double coverage = 0;                  // share of the song's sounding part covered by segments
    double score = 0;                     // mean correlation over the segments
    double margin = 0;                    // how much the chosen lags stand out from each window's next best (mean r)
    std::string verdict;                  // "same edit" / "edited" / "no match" / "drift" / "reordered" / ... (logs)
    // Each window's pick (tests, dev logs): its center, the lag chosen (or the best candidate's when none), r.
    struct WindowPick {
        double center = 0, lag = 0, r = 0;
        bool matched = false, silent = false;
    };
    std::vector<WindowPick> windows;
};

AlignResult alignAudio(const AudioFeatures& song, const AudioFeatures& video, const AlignOptions& options = {});

// The song -> video time relation an accepted alignment gives (identity when built from nothing): what the picture's
// clock is at a song position, and whether the picture shows there. Outside segments it holds: hidden, at rate 0, on
// the first frame of the next segment (the decoder seeks there meanwhile and has it ready), or after the last one on
// its end. A segment that follows a jump in the video's timeline starts `holdSec` into it (the seek's time), unless a
// gap before it is that long already; one right after a segment and less than `smallJumpSec` off it (a micro-edit)
// just goes on from there (the scheduler drops or repeats a frame or two).
class TimeMap {
public:
    TimeMap() = default;
    explicit TimeMap(std::vector<AlignSegment> segments, double holdSec = 1.2, double smallJumpSec = 0.25);
    struct Point {
        double videoSec = 0;
        double rate = 1;      // multiplies the song clock's rate (the segment's slope; 0 while holding)
        bool shown = true;
    };
    Point at(double audioSec) const;
    bool identity() const { return segments_.empty(); }
    const std::vector<AlignSegment>& segments() const { return segments_; }

private:
    std::vector<AlignSegment> segments_;   // trimmed for the holds
};

} // namespace st::video
