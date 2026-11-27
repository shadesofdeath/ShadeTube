#pragma once
// The 2048-point FFT shared by the engine's spectrum tap (audio/Spectrum) and the visualizer's analysis
// (audio/VisualAnalysis). Tables are built once (thread-safe static init); no allocation per call.
#include <cstddef>

namespace st::audio::fft {

inline constexpr size_t kSize = 2048;

// In-place iterative radix-2 decimation-in-time FFT of kSize complex values.
void forward(float* re, float* im);
// The Hann window over kSize points (a full-scale sine through it peaks at kSize / 4).
const float* hann();

} // namespace st::audio::fft
