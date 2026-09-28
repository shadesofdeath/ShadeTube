#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Videos/Streams/StreamClient.hpp>

#include "../../Bridge/Cipher/JsScanner.hpp"
#include "../../Bridge/Manifests.hpp"
#include "../../Bridge/PlayerSource.hpp"
#include "../../ClientContext.hpp"
#include "../../Utils/Json.hpp"
#include "../../Utils/StringUtils.hpp"
#include "../../Utils/Url.hpp"
#include "../VideoController.hpp"

#include <algorithm>
#include <fstream>
#include <map>

namespace YoutubeExplode::Videos::Streams {

using namespace YoutubeExplode::detail;

namespace {

/// Resolves protected stream URLs: deciphers "s" into "sig" and transforms "n".
///
/// Strategy (most to least authoritative):
///  1. JS engine + whole-player solver: runs YouTube's own code; works on modern players.
///  2. Built-in transform plan (reverse/splice/swap) for the signature: no JS engine needed.
///  3. JS engine + extracted function snippets (older players).
/// All challenges of a manifest are solved in a single engine evaluation.
class UrlResolver {
public:
    UrlResolver(ClientContext& context, const CancellationToken& ct) : context_(context), ct_(ct) {}

    void solve(const std::vector<std::string>& signatures, const std::vector<std::string>& ns) {
        if (signatures.empty() && ns.empty())
            return;
        const auto& player = source();
        const auto& engine = context_.options().jsEngine;

        if (engine && player.solverScript()) {
            std::string script = *player.solverScript();
            script += ";JSON.stringify({s:" + toJsArray(signatures) +
                      ".map(function(x){try{return _yte_solve(x).sig}catch(e){return null}}),n:" + toJsArray(ns) +
                      ".map(function(x){try{return _yte_solve(undefined,x).n}catch(e){return null}})})";
            collect(evaluate(*engine, script), signatures, ns);
        }

        std::vector<std::string> pendingSignatures, pendingNs;
        for (const auto& s : signatures)
            if (!signatures_.count(s)) pendingSignatures.push_back(s);
        for (const auto& n : ns)
            if (!ns_.count(n)) pendingNs.push_back(n);

        if (const auto& manifest = player.cipherManifest()) {
            for (const auto& s : pendingSignatures)
                signatures_[s] = manifest->decipher(s);
            pendingSignatures.clear();
        }

        if (engine && (!pendingSignatures.empty() || !pendingNs.empty())) {
            std::string script;
            std::string sPart = "[]", nPart = "[]";
            if (!pendingSignatures.empty() && player.signatureScript()) {
                script += *player.signatureScript();
                sPart = toJsArray(pendingSignatures) + ".map(function(x){try{return __yte_sig(x)}catch(e){return null}})";
            }
            if (!pendingNs.empty() && player.nScript()) {
                script += *player.nScript();
                nPart = toJsArray(pendingNs) + ".map(function(x){try{return __yte_n(x)}catch(e){return null}})";
            }
            if (!script.empty()) {
                script += ";JSON.stringify({s:" + sPart + ",n:" + nPart + "})";
                collect(evaluate(*engine, script), sPart == "[]" ? std::vector<std::string>{} : pendingSignatures,
                        nPart == "[]" ? std::vector<std::string>{} : pendingNs);
            }
        }
    }

    std::string decipherSignature(const std::string& signature) const {
        auto it = signatures_.find(signature);
        if (it != signatures_.end())
            return it->second;
        const auto& player = *player_;
        throw Exceptions::CipherExtractionException(
            "Failed to decipher the stream signature using player " + player.url() + ". " + player.diagnostics() +
            (context_.options().jsEngine
                 ? " The JavaScript engine could not solve it either."
                 : " Supplying a JavaScript engine (YoutubeClientOptions::jsEngine) enables the whole-player solver.") +
            " YouTube likely changed its player; the extraction heuristics need to be updated.");
    }

    /// Transformed n value, or the original one when it could not be solved (the URL then still
    /// plays, but may be throttled or rejected).
    std::string transformN(const std::string& n) const {
        auto it = ns_.find(n);
        return it != ns_.end() ? it->second : n;
    }

private:
    const PlayerSource& source() {
        if (!player_)
            player_ = context_.playerSource(ct_);
        return *player_;
    }

    static std::string toJsArray(const std::vector<std::string>& values) {
        std::string out = "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i) out += ",";
            out += Js::toStringLiteral(values[i]);
        }
        return out + "]";
    }

    static std::string evaluate(JavaScript::IJsEngine& engine, const std::string& script) {
        try {
            return engine.evaluate(script);
        } catch (const std::exception&) {
            return {};  // Treated as "unsolved"; fallbacks apply.
        }
    }

    void collect(const std::string& raw, const std::vector<std::string>& signatures, const std::vector<std::string>& ns) {
        auto json = tryParseJson(raw);
        if (!json)
            return;
        auto take = [](const Json* array, const std::vector<std::string>& inputs, std::map<std::string, std::string>& out,
                       bool isN) {
            if (!array || !array->is_array())
                return;
            for (std::size_t i = 0; i < inputs.size() && i < array->size(); ++i) {
                const auto& v = (*array)[i];
                if (!v.is_string())
                    continue;
                auto value = v.get<std::string>();
                // "enhanced_except_..." is the n function's own error marker.
                if (value.empty() || (isN && (startsWith(value, "enhanced_except") || value == inputs[i])))
                    continue;
                out[inputs[i]] = value;
            }
        };
        take(dig(*json, {"s"}), signatures, signatures_, false);
        take(dig(*json, {"n"}), ns, ns_, true);
    }

    ClientContext& context_;
    const CancellationToken& ct_;
    std::shared_ptr<const PlayerSource> player_;
    std::map<std::string, std::string> signatures_;
    std::map<std::string, std::string> ns_;
};

class StreamController : public VideoController {
public:
    explicit StreamController(ClientContext& context) : VideoController(context) {}

    std::vector<std::shared_ptr<const IStreamInfo>> getStreamInfos(const std::string& videoId,
                                                                   const CancellationToken& ct) {
        try {
            // Try the cipher-less clients first: their URLs are directly playable.
            auto response = getPlayerResponse(videoId, ct);
            return getStreamInfos(videoId, response, false, ct);
        } catch (const Exceptions::VideoUnavailableException&) {
            throw;
        } catch (const Exceptions::VideoRequiresPurchaseException&) {
            throw;
        } catch (const Exceptions::VideoUnplayableException&) {
            // Retry with the embedded TV client, which requires deciphering.
            const auto player = context_.playerSource(ct);
            if (!player->signatureTimestamp()) {
                throw Exceptions::CipherExtractionException(
                    "Failed to extract the signature timestamp from the player (" + player->url() + "). " +
                    player->diagnostics());
            }
            auto response = getPlayerResponseWithCipher(videoId, *player->signatureTimestamp(), ct);
            return getStreamInfos(videoId, response, true, ct);
        }
    }

    std::vector<std::shared_ptr<const IStreamInfo>> getStreamInfos(const std::string& videoId,
                                                                   const PlayerResponse& response,
                                                                   bool usesPlayerJs, const CancellationToken& ct) {
        ensurePlayable(videoId, response);

        UrlResolver resolver(context_, ct);
        std::vector<std::shared_ptr<const IStreamInfo>> result;
        append(result, response.streams(), resolver, usesPlayerJs, ct);

        if (auto dashUrl = response.dashManifestUrl()) {
            try {
                auto xml = context_.getString(*dashUrl, ct);
                append(result, parseDashManifest(xml), resolver, usesPlayerJs, ct);
            } catch (const Exceptions::HttpRequestException&) {
                // Some DASH manifest URLs return 404 for whatever reason.
            } catch (const Exceptions::OperationCanceledException&) {
                throw;
            } catch (const Exceptions::YoutubeExplodeException&) {
                // Malformed manifest: ignore, the adaptive formats above are usually complete.
            }
        }

        if (result.empty())
            throw Exceptions::VideoUnplayableException("Video '" + videoId + "' does not contain any playable streams.");
        return result;
    }

    std::optional<long long> probeContentLength(const StreamData& data, const std::string& url, const CancellationToken& ct) {
        auto contentLength = data.contentLength;

        // If content length is not available in the metadata, get it from a HEAD request.
        if (!contentLength) {
            Http::HttpRequest request;
            request.method = "HEAD";
            request.url = url;
            auto response = context_.send(request, ct);
            if (response.statusCode == 404 || response.statusCode == 403)
                return std::nullopt;
            ClientContext::ensureSuccess(response, url);
            contentLength = tryParseInt64(response.header("Content-Length").value_or(""));
        }

        if (contentLength && context_.options().validateStreamUrls && *contentLength >= 2) {
            // Streams may have mismatched content length; make sure the last byte is reachable.
            Http::HttpRequest request;
            request.url = Url::setQueryParameter(
                url, "range", std::to_string(*contentLength - 2) + "-" + std::to_string(*contentLength - 1));
            auto response = context_.send(request, ct);
            if (response.statusCode == 404 || response.statusCode == 403)
                return std::nullopt;
            ClientContext::ensureSuccess(response, request.url);
        }
        return contentLength;
    }

private:
    void append(std::vector<std::shared_ptr<const IStreamInfo>>& out, const std::vector<StreamData>& datas,
                UrlResolver& resolver, bool usesPlayerJs, const CancellationToken& ct) {
        // Pass 1: collect every challenge so they can be solved in one go.
        std::vector<std::string> signatures, ns;
        for (const auto& data : datas) {
            if (!data.url || isBlank(*data.url) || !data.itag)
                continue;
            if (data.signature && !data.signature->empty() &&
                std::find(signatures.begin(), signatures.end(), *data.signature) == signatures.end())
                signatures.push_back(*data.signature);
            if (usesPlayerJs)
                if (auto n = Url::tryGetQueryParameter(*data.url, "n");
                    n && !n->empty() && std::find(ns.begin(), ns.end(), *n) == ns.end())
                    ns.push_back(*n);
        }
        resolver.solve(signatures, ns);

        for (const auto& data : datas) {
            ct.throwIfCancellationRequested();

            // SABR / server-side streams have no progressive URL we can download.
            if (!data.url || isBlank(*data.url) || !data.itag)
                continue;
            std::string url = *data.url;

            // Cipher-protected stream
            if (data.signature && !data.signature->empty()) {
                url = Url::setQueryParameter(url, data.signatureParameter.value_or("sig"),
                                             resolver.decipherSignature(*data.signature));
            }

            // Throttling parameter (only meaningful for URLs issued to player-JS based clients).
            if (usesPlayerJs) {
                if (auto n = Url::tryGetQueryParameter(url, "n"); n && !n->empty()) {
                    auto transformed = resolver.transformN(*n);
                    if (transformed != *n)
                        url = Url::setQueryParameter(url, "n", transformed);
                }
            }

            std::optional<long long> contentLength;
            try {
                contentLength = probeContentLength(data, url, ct);
            } catch (const Exceptions::HttpRequestException&) {
                // A single unreachable stream must not fail the whole manifest.
                continue;
            }
            if (!contentLength)
                continue;

            if (!data.container || !data.bitrate)
                continue;

            StreamInfoData info;
            info.url = url;
            info.itag = *data.itag;
            info.mimeType = data.mimeType.value_or("");
            info.container = Container(*data.container);
            info.size = FileSize(*contentLength);
            info.bitrate = Bitrate(*data.bitrate);
            info.audioCodec = data.audioCodec.value_or("");
            if (data.audioLanguageCode && !isBlank(*data.audioLanguageCode))
                info.audioLanguage = Common::Language(*data.audioLanguageCode,
                                                      data.audioLanguageName.value_or(*data.audioLanguageCode));
            info.isAudioLanguageDefault = data.isAudioLanguageDefault;
            info.audioSampleRate = data.audioSampleRate;
            info.audioChannels = data.audioChannels;
            info.loudnessDb = data.loudnessDb;
            info.isDrc = data.isDrc;

            if (data.videoCodec && !data.videoCodec->empty()) {
                const int framerate = data.videoFramerate.value_or(24);
                std::optional<VideoQuality> quality;
                if (data.videoQualityLabel && !data.videoQualityLabel->empty())
                    quality = VideoQuality::tryFromLabel(*data.videoQualityLabel, framerate);
                if (!quality)
                    quality = VideoQuality::tryFromItag(*data.itag, framerate);
                if (!quality && data.videoHeight)
                    quality = VideoQuality(*data.videoHeight, framerate);
                if (!quality)
                    continue;
                info.videoCodec = *data.videoCodec;
                info.videoQuality = *quality;
                info.videoResolution = data.videoWidth && data.videoHeight
                                           ? Common::Resolution(*data.videoWidth, *data.videoHeight)
                                           : quality->getDefaultVideoResolution();
                info.isVideoUpscaled = data.isVideoUpscaled;

                if (data.audioCodec && !data.audioCodec->empty())
                    out.push_back(std::make_shared<MuxedStreamInfo>(std::move(info)));
                else
                    out.push_back(std::make_shared<VideoOnlyStreamInfo>(std::move(info)));
            } else if (data.audioCodec && !data.audioCodec->empty()) {
                out.push_back(std::make_shared<AudioOnlyStreamInfo>(std::move(info)));
            }
        }
    }
};

bool isTransient(const Exceptions::HttpRequestException& ex) { return ex.statusCode() == 0 || ex.statusCode() >= 500; }

} // namespace

StreamManifest StreamClient::getManifest(const VideoId& videoId, const CancellationToken& cancellationToken) const {
    StreamController controller(*context_);
    for (int retriesRemaining = 5;; --retriesRemaining) {
        try {
            return StreamManifest(controller.getStreamInfos(videoId.value(), cancellationToken));
        } catch (const Exceptions::HttpRequestException& ex) {
            // Retry on connectivity issues
            if (!isTransient(ex) || retriesRemaining <= 0)
                throw;
        }
    }
}

std::string StreamClient::getHttpLiveStreamUrl(const VideoId& videoId, const CancellationToken& cancellationToken) const {
    StreamController controller(*context_);
    auto response = controller.getPlayerResponse(videoId.value(), cancellationToken);
    VideoController::ensurePlayable(videoId.value(), response);
    auto url = response.hlsManifestUrl();
    if (!url || url->empty()) {
        throw Exceptions::YoutubeExplodeException("Failed to extract the HTTP Live Stream manifest URL. Video '" +
                                                  videoId.value() + "' is likely not a live stream.");
    }
    return *url;
}

MediaStream StreamClient::get(const IStreamInfo& streamInfo, const CancellationToken& cancellationToken) const {
    return MediaStream(context_, streamInfo.url(), streamInfo.size().bytes(), cancellationToken);
}

void StreamClient::copyTo(const IStreamInfo& streamInfo, std::ostream& destination, const Progress& progress,
                          const CancellationToken& cancellationToken) const {
    auto stream = get(streamInfo, cancellationToken);
    std::vector<char> buffer(256 * 1024);
    const auto total = stream.length();
    while (true) {
        cancellationToken.throwIfCancellationRequested();
        const auto read = stream.read(buffer.data(), buffer.size());
        if (read == 0)
            break;
        destination.write(buffer.data(), static_cast<std::streamsize>(read));
        if (!destination)
            throw Exceptions::YoutubeExplodeException("Failed to write to the destination stream.");
        if (progress && total > 0)
            progress(static_cast<double>(stream.position()) / static_cast<double>(total));
    }
    destination.flush();
}

void StreamClient::download(const IStreamInfo& streamInfo, const std::string& filePath, const Progress& progress,
                            const CancellationToken& cancellationToken) const {
    std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
    if (!file)
        throw Exceptions::YoutubeExplodeException("Failed to open '" + filePath + "' for writing.");
    copyTo(streamInfo, file, progress, cancellationToken);
}

} // namespace YoutubeExplode::Videos::Streams
