#include "Manifests.hpp"

#include "../Utils/StringUtils.hpp"
#include "../Utils/Url.hpp"
#include "../Utils/Xml.hpp"

#include <algorithm>
#include <regex>

namespace YoutubeExplode::detail {

std::vector<StreamData> parseDashManifest(const std::string& xml) {
    std::vector<StreamData> result;
    auto root = Xml::parse(xml);
    static const std::regex clenPattern(R"([/\?]clen[/=](\d+))");
    static const std::regex mimePattern(R"(mime[/=]\w*%2F([\w\d]*))");

    for (const auto* rep : root->descendants("Representation")) {
        const auto id = rep->attribute("id").value_or("");
        // Skip non-media representations (like "rawcc")
        if (id.empty() || !std::all_of(id.begin(), id.end(), isAsciiDigit))
            continue;
        // Skip segmented streams
        const auto inits = rep->descendants("Initialization");
        if (!inits.empty() && contains(inits.front()->attribute("sourceURL").value_or(""), "sq/"))
            continue;
        const auto codecs = rep->attribute("codecs");
        if (!codecs || isBlank(*codecs))
            continue;

        StreamData d;
        d.itag = tryParseInt(id);
        if (const auto* baseUrl = rep->element("BaseURL"))
            d.url = trim(baseUrl->innerText());
        d.contentLength = tryParseInt64(rep->attribute("contentLength").value_or(""));
        std::smatch m;
        if (!d.contentLength && d.url && std::regex_search(*d.url, m, clenPattern))
            d.contentLength = tryParseInt64(m[1].str());
        d.bitrate = tryParseInt64(rep->attribute("bandwidth").value_or(""));
        if (d.url && std::regex_search(*d.url, m, mimePattern))
            d.container = Url::decode(m[1].str());
        const bool isAudioOnly = rep->element("AudioChannelConfiguration") != nullptr;
        if (isAudioOnly) {
            d.audioCodec = codecs;
            d.audioSampleRate = tryParseInt(rep->attribute("audioSamplingRate").value_or(""));
            if (const auto* ch = rep->element("AudioChannelConfiguration"))
                d.audioChannels = tryParseInt(ch->attribute("value").value_or(""));
        } else {
            d.videoCodec = codecs;
        }
        d.videoWidth = tryParseInt(rep->attribute("width").value_or(""));
        d.videoHeight = tryParseInt(rep->attribute("height").value_or(""));
        d.videoFramerate = tryParseInt(rep->attribute("frameRate").value_or(""));
        if (d.container)
            d.mimeType = std::string(isAudioOnly ? "audio/" : "video/") + *d.container + "; codecs=\"" + *codecs + "\"";
        result.push_back(std::move(d));
    }
    return result;
}

std::vector<CaptionData> parseClosedCaptionTrack(const std::string& xml) {
    std::vector<CaptionData> result;
    auto root = Xml::parse(xml);
    auto toSpan = [](const std::optional<std::string>& ms) -> std::optional<TimeSpan> {
        if (!ms)
            return std::nullopt;
        auto v = tryParseDouble(*ms);
        if (!v)
            return std::nullopt;
        return TimeSpan(static_cast<long long>(*v));
    };
    for (const auto* p : root->descendants("p")) {
        CaptionData c;
        c.text = p->innerText();
        c.offset = toSpan(p->attribute("t"));
        c.duration = toSpan(p->attribute("d"));
        for (const auto* s : p->elements("s")) {
            CaptionPartData part;
            part.text = s->innerText();
            auto offset = toSpan(s->attribute("t"));
            if (!offset)
                offset = toSpan(s->attribute("ac"));
            part.offset = offset.value_or(TimeSpan(0));
            c.parts.push_back(std::move(part));
        }
        result.push_back(std::move(c));
    }
    return result;
}

} // namespace YoutubeExplode::detail
