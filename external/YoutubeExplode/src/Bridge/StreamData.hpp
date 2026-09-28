#pragma once

#include <optional>
#include <string>

namespace YoutubeExplode::detail {

/// Raw stream description, from a player response format or a DASH representation.
struct StreamData {
    std::optional<int> itag;
    std::optional<std::string> url;
    std::optional<std::string> signature;           // "s" of signatureCipher
    std::optional<std::string> signatureParameter;  // "sp" of signatureCipher (default "sig")
    std::optional<long long> contentLength;
    std::optional<long long> bitrate;
    std::optional<std::string> mimeType;
    std::optional<std::string> container;
    std::optional<std::string> audioCodec;
    std::optional<std::string> videoCodec;
    std::optional<std::string> audioLanguageCode;
    std::optional<std::string> audioLanguageName;
    std::optional<bool> isAudioLanguageDefault;
    std::optional<int> audioSampleRate;
    std::optional<int> audioChannels;
    /// Integrated loudness relative to YouTube's reference level (dB; positive = louder).
    std::optional<double> loudnessDb;
    std::optional<std::string> videoQualityLabel;
    std::optional<int> videoWidth;
    std::optional<int> videoHeight;
    std::optional<int> videoFramerate;
    bool isVideoUpscaled = false;
    /// True for DRC ("stable volume") audio variants.
    bool isDrc = false;
};

} // namespace YoutubeExplode::detail
