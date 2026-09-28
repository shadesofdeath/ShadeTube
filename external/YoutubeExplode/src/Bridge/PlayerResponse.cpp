#include "PlayerResponse.hpp"

#include "../Utils/Crypto.hpp"
#include "../Utils/StringUtils.hpp"
#include "../Utils/Url.hpp"

#include <cstdint>
#include <map>
#include <regex>

namespace YoutubeExplode::detail {

namespace {

// Decodes a protobuf-encoded map<string,string> (used by the "xtags" field).
std::optional<std::map<std::string, std::string>> tryDeserializeProtobufMap(const std::vector<std::uint8_t>& data) {
    auto readVarint = [&](std::size_t& i) -> std::optional<std::uint64_t> {
        std::uint64_t value = 0;
        int shift = 0;
        while (i < data.size()) {
            auto b = data[i++];
            value |= static_cast<std::uint64_t>(b & 0x7F) << shift;
            if ((b & 0x80) == 0)
                return value;
            shift += 7;
            if (shift >= 64)
                break;
        }
        return std::nullopt;
    };

    std::map<std::string, std::string> result;
    std::size_t i = 0;
    while (i < data.size()) {
        auto tag = readVarint(i);
        if (!tag || (*tag & 0x7) != 2)
            return std::nullopt;
        auto length = readVarint(i);
        if (!length || i + *length > data.size())
            return std::nullopt;
        const std::size_t entryEnd = i + static_cast<std::size_t>(*length);
        std::optional<std::string> key, value;
        std::size_t j = i;
        while (j < entryEnd) {
            auto fieldTag = readVarint(j);
            if (!fieldTag || (*fieldTag & 0x7) != 2)
                break;
            auto strLength = readVarint(j);
            if (!strLength || j + *strLength > entryEnd)
                break;
            std::string str(data.begin() + static_cast<std::ptrdiff_t>(j),
                            data.begin() + static_cast<std::ptrdiff_t>(j + *strLength));
            j += static_cast<std::size_t>(*strLength);
            if ((*fieldTag >> 3) == 1) key = str;
            else if ((*fieldTag >> 3) == 2) value = str;
        }
        if (key)
            result[*key] = value.value_or("");
        i = entryEnd;
    }
    return result;
}

} // namespace

std::optional<std::string> PlayerResponse::playabilityStatus() const {
    return jsonString(dig(playability(), {"status"}));
}

std::optional<std::string> PlayerResponse::playabilityError() const {
    if (auto reason = jsonString(dig(playability(), {"reason"})))
        return reason;
    if (auto reason = jsonText(dig(playability(), {"errorScreen", "playerErrorMessageRenderer", "reason"})))
        return reason;
    return jsonText(dig(playability(), {"errorScreen", "playerErrorMessageRenderer", "subreason"}));
}

bool PlayerResponse::isAvailable() const {
    auto status = playabilityStatus();
    return !(status && iequals(*status, "error")) && details() != nullptr;
}

bool PlayerResponse::isPlayable() const {
    auto status = playabilityStatus();
    return status && iequals(*status, "ok");
}

bool PlayerResponse::isAgeRestricted() const {
    const auto status = toLower(playabilityStatus().value_or(""));
    if (status == "age_check_required" || status == "age_verification_required")
        return true;
    if (status != "login_required" && status != "unplayable" && status != "content_check_required")
        return false;
    const auto reason = toLower(playabilityError().value_or(""));
    if (contains(reason, "your age") || contains(reason, "age-restricted") || contains(reason, "age restricted") ||
        contains(reason, "inappropriate"))
        return true;
    return playability() && findFirstDescendant(*playability(), "desktopLegacyAgeGateReason") != nullptr;
}

std::optional<std::string> PlayerResponse::title() const { return jsonString(dig(details(), {"title"})); }
std::optional<std::string> PlayerResponse::channelId() const { return jsonString(dig(details(), {"channelId"})); }
std::optional<std::string> PlayerResponse::author() const { return jsonString(dig(details(), {"author"})); }

std::optional<DateTimeOffset> PlayerResponse::uploadDate() const {
    for (const char* key : {"uploadDate", "publishDate"}) {
        if (auto s = jsonString(dig(content_, {"microformat", "playerMicroformatRenderer", key})))
            if (auto d = tryParseIso8601(*s))
                return d;
    }
    return std::nullopt;
}

std::optional<TimeSpan> PlayerResponse::duration() const {
    auto seconds = jsonInt64(dig(details(), {"lengthSeconds"}));
    if (!seconds || isLive())
        return std::nullopt;
    return std::chrono::duration_cast<TimeSpan>(std::chrono::seconds(*seconds));
}

std::vector<ThumbnailData> PlayerResponse::thumbnails() const { return parseThumbnails(dig(details(), {"thumbnail"})); }

std::vector<std::string> PlayerResponse::keywords() const {
    std::vector<std::string> result;
    if (auto array = dig(details(), {"keywords"}); array && array->is_array())
        for (const auto& k : *array)
            if (k.is_string())
                result.push_back(k.get<std::string>());
    return result;
}

std::optional<std::string> PlayerResponse::description() const { return jsonString(dig(details(), {"shortDescription"})); }
std::optional<long long> PlayerResponse::viewCount() const { return jsonInt64(dig(details(), {"viewCount"})); }

bool PlayerResponse::isLive() const {
    // Currently live, or an upcoming premiere/stream. Finished streams (isLiveContent only) are VODs.
    return jsonBool(dig(details(), {"isLive"})).value_or(false) ||
           jsonBool(dig(details(), {"isUpcoming"})).value_or(false);
}

std::optional<std::string> PlayerResponse::previewVideoId() const {
    if (auto id = jsonString(dig(playability(), {"errorScreen", "playerLegacyDesktopYpcTrailerRenderer", "trailerVideoId"})))
        return id;
    if (auto vars = jsonString(dig(playability(), {"errorScreen", "ypcTrailerRenderer", "playerVars"})))
        if (auto id = Url::tryGetQueryParameter(*vars, "video_id"))
            return id;
    if (auto encoded = jsonString(dig(playability(), {"errorScreen", "ypcTrailerRenderer", "playerResponse"}))) {
        // Base64-like blob; enough of it decodes correctly to find the ID.
        if (auto bytes = base64Decode(*encoded)) {
            std::string text(bytes->begin(), bytes->end());
            static const std::regex pattern(R"(video_id=(.{11}))");
            std::smatch m;
            if (std::regex_search(text, m, pattern))
                return m[1].str();
        }
    }
    return std::nullopt;
}

std::optional<std::string> PlayerResponse::dashManifestUrl() const {
    return jsonString(dig(content_, {"streamingData", "dashManifestUrl"}));
}

std::optional<std::string> PlayerResponse::hlsManifestUrl() const {
    return jsonString(dig(content_, {"streamingData", "hlsManifestUrl"}));
}

StreamData parseStreamData(const Json& format) {
    StreamData d;
    d.itag = jsonInt(dig(format, {"itag"}));

    std::optional<std::string> cipher = jsonString(dig(format, {"signatureCipher"}));
    if (!cipher)
        cipher = jsonString(dig(format, {"cipher"}));

    d.url = jsonString(dig(format, {"url"}));
    if (cipher) {
        if (!d.url)
            d.url = Url::tryGetQueryParameter(*cipher, "url");
        d.signature = Url::tryGetQueryParameter(*cipher, "s");
        d.signatureParameter = Url::tryGetQueryParameter(*cipher, "sp");
    }

    d.contentLength = jsonInt64(dig(format, {"contentLength"}));
    if (!d.contentLength && d.url)
        d.contentLength = tryParseInt64(Url::tryGetQueryParameter(*d.url, "clen").value_or(""));

    d.bitrate = jsonInt64(dig(format, {"bitrate"}));
    d.mimeType = jsonString(dig(format, {"mimeType"}));
    if (d.mimeType) {
        const auto& mime = *d.mimeType;
        d.container = substringAfter(substringUntil(mime, ";"), "/");
        const bool isAudioOnly = startsWith(toLower(mime), "audio/");
        const auto codecs = substringUntil(substringAfter(mime, "codecs=\""), "\"");
        if (isAudioOnly) {
            if (!codecs.empty()) d.audioCodec = codecs;
        } else {
            auto video = trim(substringUntil(codecs, ","));
            auto audio = trim(substringAfter(codecs, ","));
            if (iequals(video, "unknown"))
                video = "av01.0.05M.08";  // "unknown" indicates av01
            if (!video.empty()) d.videoCodec = video;
            if (!audio.empty()) d.audioCodec = audio;
        }
    }

    if (auto trackId = jsonString(dig(format, {"audioTrack", "id"})))
        d.audioLanguageCode = substringUntil(*trackId, ".");
    d.audioLanguageName = jsonString(dig(format, {"audioTrack", "displayName"}));
    d.isAudioLanguageDefault = jsonBool(dig(format, {"audioTrack", "audioIsDefault"}));
    d.audioSampleRate = jsonInt(dig(format, {"audioSampleRate"}));
    d.audioChannels = jsonInt(dig(format, {"audioChannels"}));
    d.loudnessDb = jsonDouble(dig(format, {"loudnessDb"}));
    d.isDrc = jsonBool(dig(format, {"isDrc"})).value_or(false);

    d.videoQualityLabel = jsonString(dig(format, {"qualityLabel"}));
    d.videoWidth = jsonInt(dig(format, {"width"}));
    d.videoHeight = jsonInt(dig(format, {"height"}));
    d.videoFramerate = jsonInt(dig(format, {"fps"}));

    // xtags is a base64 protobuf map<string,string>; Super Resolution streams carry {"sr": "1"}.
    if (auto xtags = jsonString(dig(format, {"xtags"})); xtags && !xtags->empty()) {
        if (auto bytes = base64Decode(*xtags))
            if (auto map = tryDeserializeProtobufMap(*bytes)) {
                auto it = map->find("sr");
                d.isVideoUpscaled = it != map->end() && it->second == "1";
                auto drc = map->find("drc");
                if (drc != map->end() && drc->second == "1")
                    d.isDrc = true;
            }
    }
    return d;
}

std::vector<StreamData> PlayerResponse::streams() const {
    std::vector<StreamData> result;
    for (const char* key : {"formats", "adaptiveFormats"}) {
        if (auto array = dig(content_, {"streamingData", key}); array && array->is_array())
            for (const auto& format : *array)
                result.push_back(parseStreamData(format));
    }
    // Older / other clients only report the loudness once, for the whole video.
    if (auto loudness = jsonDouble(dig(content_, {"playerConfig", "audioConfig", "loudnessDb"})))
        for (auto& d : result)
            if (!d.loudnessDb)
                d.loudnessDb = loudness;
    return result;
}

std::vector<ClosedCaptionTrackData> PlayerResponse::closedCaptionTracks() const {
    std::vector<ClosedCaptionTrackData> result;
    auto tracks = dig(content_, {"captions", "playerCaptionsTracklistRenderer", "captionTracks"});
    if (!tracks || !tracks->is_array())
        return result;
    for (const auto& t : *tracks) {
        ClosedCaptionTrackData d;
        d.url = jsonString(dig(t, {"baseUrl"}));
        d.languageCode = jsonString(dig(t, {"languageCode"}));
        d.languageName = jsonText(dig(t, {"name"}));
        auto vssId = jsonString(dig(t, {"vssId"}));
        d.isAutoGenerated = (vssId && startsWith(toLower(*vssId), "a.")) ||
                            jsonString(dig(t, {"kind"})).value_or("") == "asr";
        result.push_back(std::move(d));
    }
    return result;
}

} // namespace YoutubeExplode::detail
