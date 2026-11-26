#pragma once
// The fragmented MP4 index lives in st_audio (audio/Fmp4Index: the engine frees edit lists with it too); video mode
// uses it under its own namespace.
#include "audio/Fmp4Index.h"

namespace st::video {
using audio::Fmp4Index;
using audio::Fmp4Segment;
using audio::parseFmp4Index;
} // namespace st::video
