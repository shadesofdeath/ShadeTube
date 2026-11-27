#pragma once
// Tempo (BPM) of music, estimated in-tree from the audio (no dependency, no network): the app's own analysis for songs
// whose files carry no BPM tag (app/Bpm). Standalone (st_audio; tests/tempo).
//
// Method:
//   1. Onset strength envelope: mono audio at kRate (11.025 kHz), 512-point Hann frames every 128 samples (86.1 frames a
//      second), magnitudes summed into 20 log-spaced bands (40 Hz .. 5 kHz), log-compressed; the envelope is the
//      positive band-wise difference to the previous frame (spectral flux), minus its 1 s moving average, rectified.
//   2. Autocorrelation of the envelope (lags up to ~5 s), normalized by the zero lag.
//   3. Comb score of every period P between kMaxBpm and kMinBpm (steps of 1/8 frame, linear interpolation): the
//      autocorrelation at P, 2P, 3P and 4P (weights 1, 0.8, 0.6, 0.4), times a gentle log-normal prior centered on
//      120 BPM (1.4 octaves wide), so a tie between octaves goes to the usual tactus.
//   4. Octave check of the winner: when the events halfway between its beats are about as strong as the beats
//      (the autocorrelation at P/2 at least 0.85 of the one at P) and twice the tempo stays in range, the beats are
//      twice as fast (a 200 BPM click is not "100 BPM with a click in between"); when the envelope repeats at 2P much
//      more strongly than at P (under 0.55 of it), the winner was a subdivision and the tempo halves.
//   5. The period is refined to the best comb score within one frame (steps of 1/32 frame), and the confidence is the
//      autocorrelation at it (0..1). Below kMinConfidence there is no steady beat: bpm 0.
// Octave errors (half / double time) remain possible on real music, as with every tempo estimator; tests/tempo
// measures them on synthetic loops and the guide (docs/DEVELOPMENT.md) reports real songs.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace st::audio::tempo {

inline constexpr uint32_t kRate = 11025;       // analysis sample rate (analyzeFile decodes to it)
inline constexpr double kMinBpm = 50, kMaxBpm = 210;
inline constexpr double kMinConfidence = 0.08;

struct Estimate {
    double bpm = 0;          // beats per minute (0.1 resolution), 0 = no steady beat found
    double confidence = 0;   // 0..1: how strongly the onset envelope repeats at that period
};

// The onset strength envelope of mono samples (any rate; frames of ~46 ms every ~11.6 ms) and its frame rate.
std::vector<float> onsetEnvelope(const float* mono, size_t frames, uint32_t rate, double& frameRate);
// Steps 2-5 on an envelope (at least 6 s of it, else no estimate).
Estimate fromEnvelope(const std::vector<float>& env, double frameRate);
// onsetEnvelope + fromEnvelope.
Estimate estimate(const float* mono, size_t frames, uint32_t rate);

struct FileResult {
    bool ok = false;           // the file decoded (also when no beat was found: tempo.bpm == 0)
    Estimate tempo;
    double seconds = 0;        // audio analysed
    double decodeMs = 0;       // wall time of the decode
    double analyzeMs = 0;      // wall time of the estimate
    std::string error;         // when !ok (log text)
};
// Blocking (worker thread; initializes COM and Media Foundation itself): decodes up to `maxSeconds` of a local file
// (any format the engine plays: the MIME type as app/Player's localMimeType gives it, "" = sniffed) from 20 % of its
// length (songs shorter than maxSeconds whole), mono at kRate, and estimates its tempo. `cancel` set = stop early.
FileResult analyzeFile(const std::wstring& path, const std::string& mimeType, double maxSeconds = 70,
                       const std::atomic<bool>* cancel = nullptr);

} // namespace st::audio::tempo
