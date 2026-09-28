#include <YoutubeExplode/Videos/Streams/Bitrate.hpp>
#include <YoutubeExplode/Videos/Streams/Container.hpp>
#include <YoutubeExplode/Videos/Streams/FileSize.hpp>
#include <YoutubeExplode/Videos/Streams/StreamManifest.hpp>
#include <YoutubeExplode/Videos/Streams/VideoQuality.hpp>

#include "../../Utils/StringUtils.hpp"

#include <cmath>
#include <cstdio>
#include <regex>

namespace YoutubeExplode::Videos::Streams {

namespace {

std::string formatScaled(double value, const char* unit) {
    // "0.##" formatting: up to two decimals, trailing zeros trimmed.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.2f", value);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s + " " + unit;
}

} // namespace

bool Container::isAudioOnly() const {
    for (const char* n : {"mp3", "m4a", "wav", "wma", "ogg", "aac", "opus"})
        if (detail::iequals(name_, n))
            return true;
    return false;
}

bool operator==(const Container& a, const Container& b) { return detail::iequals(a.name_, b.name_); }

std::string Bitrate::toString() const {
    if (std::abs(gigaBitsPerSecond()) >= 1) return formatScaled(gigaBitsPerSecond(), "Gbit/s");
    if (std::abs(megaBitsPerSecond()) >= 1) return formatScaled(megaBitsPerSecond(), "Mbit/s");
    if (std::abs(kiloBitsPerSecond()) >= 1) return formatScaled(kiloBitsPerSecond(), "Kbit/s");
    return formatScaled(static_cast<double>(bps_), "Bit/s");
}

std::string FileSize::toString() const {
    if (std::abs(gigaBytes()) >= 1) return formatScaled(gigaBytes(), "GB");
    if (std::abs(megaBytes()) >= 1) return formatScaled(megaBytes(), "MB");
    if (std::abs(kiloBytes()) >= 1) return formatScaled(kiloBytes(), "KB");
    return formatScaled(static_cast<double>(bytes_), "B");
}

// ---- VideoQuality ---------------------------------------------------------------------------------

static std::string formatQualityLabel(int maxHeight, int framerate) {
    // Framerate appears only if it's above 30; YouTube rounds it up to the nearest 10.
    if (framerate <= 30)
        return std::to_string(maxHeight) + "p";
    const int rounded = static_cast<int>(std::ceil(framerate / 10.0)) * 10;
    return std::to_string(maxHeight) + "p" + std::to_string(rounded);
}

VideoQuality::VideoQuality(int maxHeight, int framerate)
    : label_(formatQualityLabel(maxHeight, framerate)), maxHeight_(maxHeight), framerate_(framerate) {}

std::optional<VideoQuality> VideoQuality::tryFromLabel(const std::string& label, int framerateFallback) {
    // 1080p, 1080p60, 1080s (360°), 1080s60, 2160p60 HDR
    static const std::regex pattern(R"(^(\d+)\D(\d+)?)");
    std::smatch m;
    if (!std::regex_search(label, m, pattern))
        return std::nullopt;
    auto height = detail::tryParseInt(m[1].str());
    if (!height)
        return std::nullopt;
    auto framerate = m[2].matched ? detail::tryParseInt(m[2].str()) : std::nullopt;
    return VideoQuality(label, *height, framerate.value_or(framerateFallback));
}

std::optional<VideoQuality> VideoQuality::tryFromItag(int itag, int framerate) {
    int h = 0;
    switch (itag) {
        case 5: case 13: case 17: case 91: case 151: case 160: case 161: case 278: case 330: case 394: h = 144; break;
        case 6: case 36: case 92: case 132: case 133: case 142: case 242: case 331: case 395: h = 240; break;
        case 18: case 34: case 43: case 82: case 93: case 100: case 134: case 143: case 167: case 243: case 332:
        case 396: h = 360; break;
        case 35: case 44: case 59: case 78: case 83: case 94: case 101: case 135: case 144: case 168: case 212:
        case 213: case 218: case 219: case 222: case 223: case 244: case 245: case 246: case 333: case 397: h = 480; break;
        case 22: case 45: case 84: case 95: case 102: case 136: case 145: case 169: case 214: case 215: case 224:
        case 225: case 247: case 298: case 302: case 334: case 398: h = 720; break;
        case 37: case 46: case 85: case 96: case 137: case 146: case 170: case 216: case 217: case 226: case 227:
        case 248: case 299: case 303: case 335: case 399: h = 1080; break;
        case 264: case 271: case 308: case 336: h = 1440; break;
        case 266: case 272: case 313: case 315: case 337: h = 2160; break;
        case 38: h = 3072; break;
        case 138: h = 4320; break;
        default: return std::nullopt;
    }
    return VideoQuality(h, framerate);
}

Common::Resolution VideoQuality::getDefaultVideoResolution() const {
    switch (maxHeight_) {
        case 144: return {256, 144};
        case 240: return {426, 240};
        case 360: return {640, 360};
        case 480: return {854, 480};
        case 720: return {1280, 720};
        case 1080: return {1920, 1080};
        case 1440: return {2560, 1440};
        case 2160: return {3840, 2160};
        case 2880: return {5120, 2880};
        case 3072: return {4096, 3072};
        case 4320: return {7680, 4320};
        default: return {16 * maxHeight_ / 9, maxHeight_};
    }
}

int VideoQuality::compare(const VideoQuality& other) const {
    if (maxHeight_ != other.maxHeight_)
        return maxHeight_ < other.maxHeight_ ? -1 : 1;
    if (framerate_ != other.framerate_)
        return framerate_ < other.framerate_ ? -1 : 1;
    const auto a = detail::toLower(label_), b = detail::toLower(other.label_);
    return a < b ? -1 : (a > b ? 1 : 0);
}

// ---- Stream infos ---------------------------------------------------------------------------------

std::string AudioOnlyStreamInfo::toString() const {
    return "Audio-only (" + std::to_string(d_.itag) + " | " + d_.container.name() + " | " + d_.audioCodec + " | " +
           d_.bitrate.toString() + ")";
}

std::string VideoOnlyStreamInfo::toString() const {
    return "Video-only (" + std::to_string(d_.itag) + " | " + d_.videoQuality.label() + " | " + d_.container.name() +
           ")";
}

std::string MuxedStreamInfo::toString() const {
    return "Muxed (" + std::to_string(d_.itag) + " | " + d_.videoQuality.label() + " | " + d_.container.name() + ")";
}

// ---- Manifest -------------------------------------------------------------------------------------

std::shared_ptr<const AudioOnlyStreamInfo> StreamManifest::tryGetBestAudioOnlyStream(const std::string& container) const {
    std::shared_ptr<const AudioOnlyStreamInfo> best;
    auto rank = [](const AudioOnlyStreamInfo& s) {
        // Prefer the default audio track (or tracks without language info) over dubs.
        return s.isAudioLanguageDefault().value_or(true) ? 1 : 0;
    };
    for (const auto& s : getAudioOnlyStreams()) {
        if (!container.empty() && !detail::iequals(s->container().name(), container))
            continue;
        if (!best || rank(*s) > rank(*best) || (rank(*s) == rank(*best) && s->bitrate() > best->bitrate()))
            best = s;
    }
    return best;
}

} // namespace YoutubeExplode::Videos::Streams
