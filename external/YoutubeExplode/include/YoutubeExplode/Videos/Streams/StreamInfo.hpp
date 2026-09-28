#pragma once

#include <YoutubeExplode/Common/Language.hpp>
#include <YoutubeExplode/Common/Resolution.hpp>
#include <YoutubeExplode/Videos/Streams/Bitrate.hpp>
#include <YoutubeExplode/Videos/Streams/Container.hpp>
#include <YoutubeExplode/Videos/Streams/FileSize.hpp>
#include <YoutubeExplode/Videos/Streams/VideoQuality.hpp>

#include <optional>
#include <string>

namespace YoutubeExplode::Videos::Streams {

/// Metadata associated with a media stream of a YouTube video.
class IStreamInfo {
public:
    virtual ~IStreamInfo() = default;

    /// Directly playable/downloadable URL (signature and n-parameter already resolved).
    virtual const std::string& url() const = 0;
    virtual const Container& container() const = 0;
    virtual FileSize size() const = 0;
    virtual Bitrate bitrate() const = 0;

    /// YouTube format identifier.
    virtual int itag() const = 0;
    /// Raw MIME type, e.g. `audio/webm; codecs="opus"`.
    virtual const std::string& mimeType() const = 0;

    virtual std::string toString() const = 0;
};

/// Metadata associated with a media stream that contains audio.
class IAudioStreamInfo : public virtual IStreamInfo {
public:
    /// E.g. "opus", "mp4a.40.2".
    virtual const std::string& audioCodec() const = 0;
    /// Language of the audio track (multi-language videos only).
    virtual const std::optional<Common::Language>& audioLanguage() const = 0;
    /// Whether this is the default audio track (multi-language videos only).
    virtual std::optional<bool> isAudioLanguageDefault() const = 0;
    /// Sample rate in Hz, e.g. 48000.
    virtual std::optional<int> audioSampleRate() const = 0;
    /// Number of audio channels, e.g. 2.
    virtual std::optional<int> audioChannels() const = 0;
    /// Integrated loudness relative to YouTube's reference level in dB (positive = louder than the reference;
    /// YouTube's own player turns it down by this much). Absent when the response doesn't carry it.
    virtual std::optional<double> loudnessDb() const = 0;
    /// Whether this is a DRC ("stable volume") variant with compressed dynamics.
    virtual bool isDrc() const = 0;
};

/// Metadata associated with a media stream that contains video.
class IVideoStreamInfo : public virtual IStreamInfo {
public:
    /// E.g. "avc1.640028", "vp9", "av01.0.08M.08".
    virtual const std::string& videoCodec() const = 0;
    virtual const VideoQuality& videoQuality() const = 0;
    virtual const Common::Resolution& videoResolution() const = 0;
    /// Whether the stream was upscaled with YouTube's "Super Resolution" feature.
    virtual bool isVideoUpscaled() const = 0;
};

/// Plain data used to build stream info objects.
struct StreamInfoData {
    std::string url;
    int itag = 0;
    std::string mimeType;
    Container container;
    FileSize size;
    Bitrate bitrate;

    std::string audioCodec;
    std::optional<Common::Language> audioLanguage;
    std::optional<bool> isAudioLanguageDefault;
    std::optional<int> audioSampleRate;
    std::optional<int> audioChannels;
    std::optional<double> loudnessDb;
    bool isDrc = false;

    std::string videoCodec;
    VideoQuality videoQuality;
    Common::Resolution videoResolution;
    bool isVideoUpscaled = false;
};

/// Audio-only stream (the typical choice for music playback: opus/webm or mp4a/m4a).
class AudioOnlyStreamInfo final : public IAudioStreamInfo {
public:
    explicit AudioOnlyStreamInfo(StreamInfoData data) : d_(std::move(data)) {}

    const std::string& url() const override { return d_.url; }
    const Container& container() const override { return d_.container; }
    FileSize size() const override { return d_.size; }
    Bitrate bitrate() const override { return d_.bitrate; }
    int itag() const override { return d_.itag; }
    const std::string& mimeType() const override { return d_.mimeType; }
    const std::string& audioCodec() const override { return d_.audioCodec; }
    const std::optional<Common::Language>& audioLanguage() const override { return d_.audioLanguage; }
    std::optional<bool> isAudioLanguageDefault() const override { return d_.isAudioLanguageDefault; }
    std::optional<int> audioSampleRate() const override { return d_.audioSampleRate; }
    std::optional<int> audioChannels() const override { return d_.audioChannels; }
    std::optional<double> loudnessDb() const override { return d_.loudnessDb; }
    bool isDrc() const override { return d_.isDrc; }
    std::string toString() const override;

private:
    StreamInfoData d_;
};

/// Video-only stream (must be combined with an audio stream for playback with sound).
class VideoOnlyStreamInfo final : public IVideoStreamInfo {
public:
    explicit VideoOnlyStreamInfo(StreamInfoData data) : d_(std::move(data)) {}

    const std::string& url() const override { return d_.url; }
    const Container& container() const override { return d_.container; }
    FileSize size() const override { return d_.size; }
    Bitrate bitrate() const override { return d_.bitrate; }
    int itag() const override { return d_.itag; }
    const std::string& mimeType() const override { return d_.mimeType; }
    const std::string& videoCodec() const override { return d_.videoCodec; }
    const VideoQuality& videoQuality() const override { return d_.videoQuality; }
    const Common::Resolution& videoResolution() const override { return d_.videoResolution; }
    bool isVideoUpscaled() const override { return d_.isVideoUpscaled; }
    std::string toString() const override;

private:
    StreamInfoData d_;
};

/// Stream that contains both audio and video.
class MuxedStreamInfo final : public IAudioStreamInfo, public IVideoStreamInfo {
public:
    explicit MuxedStreamInfo(StreamInfoData data) : d_(std::move(data)) {}

    const std::string& url() const override { return d_.url; }
    const Container& container() const override { return d_.container; }
    FileSize size() const override { return d_.size; }
    Bitrate bitrate() const override { return d_.bitrate; }
    int itag() const override { return d_.itag; }
    const std::string& mimeType() const override { return d_.mimeType; }
    const std::string& audioCodec() const override { return d_.audioCodec; }
    const std::optional<Common::Language>& audioLanguage() const override { return d_.audioLanguage; }
    std::optional<bool> isAudioLanguageDefault() const override { return d_.isAudioLanguageDefault; }
    std::optional<int> audioSampleRate() const override { return d_.audioSampleRate; }
    std::optional<int> audioChannels() const override { return d_.audioChannels; }
    std::optional<double> loudnessDb() const override { return d_.loudnessDb; }
    bool isDrc() const override { return d_.isDrc; }
    const std::string& videoCodec() const override { return d_.videoCodec; }
    const VideoQuality& videoQuality() const override { return d_.videoQuality; }
    const Common::Resolution& videoResolution() const override { return d_.videoResolution; }
    bool isVideoUpscaled() const override { return d_.isVideoUpscaled; }
    std::string toString() const override;

private:
    StreamInfoData d_;
};

} // namespace YoutubeExplode::Videos::Streams
