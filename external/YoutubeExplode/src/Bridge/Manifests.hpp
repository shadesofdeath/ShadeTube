#pragma once

#include "StreamData.hpp"

#include <YoutubeExplode/Common/Time.hpp>

#include <optional>
#include <string>
#include <vector>

namespace YoutubeExplode::detail {

/// Streams listed in a DASH (MPD) manifest.
std::vector<StreamData> parseDashManifest(const std::string& xml);

struct CaptionPartData {
    std::string text;
    TimeSpan offset{0};
};

struct CaptionData {
    std::string text;
    std::optional<TimeSpan> offset;
    std::optional<TimeSpan> duration;
    std::vector<CaptionPartData> parts;
};

/// Captions from a timedtext response in format 3 (srv3 XML).
std::vector<CaptionData> parseClosedCaptionTrack(const std::string& xml);

} // namespace YoutubeExplode::detail
