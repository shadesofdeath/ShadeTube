#include <YoutubeExplode/Exceptions.hpp>
#include <YoutubeExplode/Music/TrackMatcher.hpp>

#include "../Utils/StringUtils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <unordered_set>

namespace YoutubeExplode::Music {

using namespace YoutubeExplode::detail;

namespace {

// ---- Text normalization -------------------------------------------------------------------------

/// Lower-cases ASCII, turns ASCII punctuation into spaces and collapses whitespace.
/// Non-ASCII bytes (UTF-8 letters such as ç, ş, é, 日) are kept as part of words.
std::string normalize(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    bool space = true;
    for (unsigned char c : s) {
        const bool word = c >= 0x80 || isAsciiAlnum(static_cast<char>(c));
        if (word) {
            out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
            space = false;
        } else if (!space) {
            out += ' ';
            space = true;
        }
    }
    if (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}

std::vector<std::string> tokens(std::string_view s) {
    std::vector<std::string> result;
    for (auto& t : split(normalize(s), ' '))
        if (!t.empty())
            result.push_back(std::move(t));
    return result;
}

std::string compact(std::string_view s) { return replaceAll(normalize(s), " ", ""); }

/// Whether `phrase` (normalized) occurs as whole words in `text` (normalized).
bool containsPhrase(const std::string& text, const std::string& phrase) {
    return (" " + text + " ").find(" " + phrase + " ") != std::string::npos;
}

/// Title without decorations: "Around the World (feat. X) [Remastered] - Radio Edit" -> "Around the World".
std::string coreTitle(const std::string& title) {
    std::string out;
    int depth = 0;
    for (char c : title) {
        if (c == '(' || c == '[') { ++depth; continue; }
        if (c == ')' || c == ']') { if (depth > 0) --depth; continue; }
        if (depth == 0) out += c;
    }
    // " - Remastered 2011", " - Radio Edit", " - From \"Movie\""
    auto dash = out.find(" - ");
    if (dash != std::string::npos && dash > 0)
        out.resize(dash);
    // "Song feat. Someone" / "Song ft. Someone"
    const auto lower = toLower(out);
    for (const char* marker : {" feat. ", " feat ", " ft. ", " ft ", " featuring "}) {
        auto pos = lower.find(marker);
        if (pos != std::string::npos) {
            out.resize(pos);
            break;
        }
    }
    auto trimmed = trim(out);
    return trimmed.empty() ? trim(title) : trimmed;
}

// Words/phrases that indicate a different version than the catalog track, unless the catalog
// title itself contains them (e.g. a Spotify track that *is* a live recording or a remix).
const char* const kUnwantedPhrases[] = {
    "live", "live at", "concert", "cover", "remix", "karaoke", "instrumental", "sped up", "speed up",
    "slowed", "reverb", "8d", "8d audio", "nightcore", "acoustic", "reaction", "tutorial", "lesson",
    "1 hour", "10 hours", "1 saat", "loop", "bass boosted", "extended", "mashup", "piano version",
    "drum cover", "guitar cover", "parody", "teaser", "trailer", "behind the scenes", "fan made",
    "shorts", "edit audio", "mix", "megamix", "medley",
};

} // namespace

std::string TrackMatcher::buildQuery(const TrackQuery& query) {
    const auto title = coreTitle(query.title);
    if (query.artists.empty() || trim(query.artists.front()).empty())
        return title;
    return trim(query.artists.front()) + " - " + title;
}

std::vector<TrackCandidate> TrackMatcher::rank(const TrackQuery& query,
                                               const std::vector<Search::VideoSearchResult>& results) const {
    const auto core = coreTitle(query.title);
    const auto coreTokens = tokens(core);
    const auto fullQueryText = normalize(query.title + " " + query.album);
    const auto primaryArtist = query.artists.empty() ? std::string() : query.artists.front();
    const auto primaryArtistTokens = tokens(primaryArtist);
    const auto primaryArtistCompact = compact(primaryArtist);

    std::vector<TrackCandidate> candidates;
    std::unordered_set<std::string> seen;
    std::size_t index = 0;

    for (const auto& video : results) {
        if (!seen.insert(video.id().value()).second)
            continue;
        TrackCandidate c{video, 0, {}};
        const auto titleText = normalize(video.title());
        const auto channelText = normalize(video.author().channelTitle());
        const auto titleAndChannel = titleText + " " + channelText;
        auto note = [&](const std::string& reason, double delta) {
            c.score += delta;
            char buf[32];
            std::snprintf(buf, sizeof buf, " (%+.0f)", delta);
            c.reasons.push_back(reason + buf);
        };

        // Title similarity: share of the catalog title's words present in the video title.
        if (!coreTokens.empty()) {
            std::size_t matched = 0;
            for (const auto& t : coreTokens)
                if (containsPhrase(titleText, t))
                    ++matched;
            const double ratio = static_cast<double>(matched) / static_cast<double>(coreTokens.size());
            note("title " + std::to_string(static_cast<int>(ratio * 100)) + "%", 30.0 * ratio);
            // Same artist and length but another song is the most dangerous false positive.
            if (ratio < 0.5)
                note("title mismatch", -40);
        }

        // Primary artist in the title or channel name ("Daft Punk", "Daft Punk - Topic", "DaftPunkVEVO").
        bool artistMatched = false;
        if (!primaryArtistTokens.empty()) {
            std::size_t matched = 0;
            for (const auto& t : primaryArtistTokens)
                if (containsPhrase(titleAndChannel, t))
                    ++matched;
            double ratio = static_cast<double>(matched) / static_cast<double>(primaryArtistTokens.size());
            if (!primaryArtistCompact.empty() && compact(video.author().channelTitle()).find(primaryArtistCompact) != std::string::npos)
                ratio = 1.0;
            artistMatched = ratio >= 0.99;
            note("artist " + std::to_string(static_cast<int>(ratio * 100)) + "%", 25.0 * ratio);
        }
        // Featured artists: small bonus each.
        double featured = 0;
        for (std::size_t i = 1; i < query.artists.size() && featured < 6; ++i) {
            const auto artist = normalize(query.artists[i]);
            if (!artist.empty() && containsPhrase(titleAndChannel, artist))
                featured += 3;
        }
        if (featured > 0)
            note("featured artists", featured);

        // Duration: the strongest signal to reject music videos with intros, loops and compilations.
        if (!video.duration()) {
            note("no duration (live stream)", -40);
        } else if (query.duration) {
            const double diff = std::abs(std::chrono::duration<double>(*video.duration() - *query.duration).count());
            double score;
            if (diff <= 3) score = 1.0;
            else if (diff <= 10) score = 1.0 - (diff - 3) / 14.0;       // 1.0 .. 0.5
            else if (diff <= 30) score = 0.5 * (30 - diff) / 20.0;      // 0.5 .. 0
            else score = 0;
            note("duration diff " + std::to_string(static_cast<int>(diff)) + "s", 30.0 * score);
            if (diff > 30)
                note("duration far off", -25);
        } else {
            note("duration unknown", 15);
        }

        // Channel and title hints.
        const auto channelLower = toLower(video.author().channelTitle());
        if (artistMatched && endsWith(channelLower, " - topic"))
            note("official Topic channel", 12);
        else if (artistMatched && contains(channelLower, "vevo"))
            note("VEVO channel", 6);
        else if (artistMatched && !primaryArtistCompact.empty() && compact(video.author().channelTitle()) == primaryArtistCompact)
            note("artist's own channel", 8);
        if (containsPhrase(titleText, "official audio"))
            note("official audio", 8);
        else if (containsPhrase(titleText, "audio"))
            note("audio", 3);
        if (containsPhrase(titleText, "official video") || containsPhrase(titleText, "official music video"))
            note("official video", 3);
        if (containsPhrase(titleText, "lyrics") || containsPhrase(titleText, "lyric video"))
            note("lyrics", 2);

        // Unwanted variants not requested by the catalog title.
        int unwanted = 0;
        for (const char* phrase : kUnwantedPhrases) {
            if (containsPhrase(titleText, phrase) && !containsPhrase(fullQueryText, phrase)) {
                note(std::string("unwanted '") + phrase + "'", unwanted == 0 ? -25 : -10);
                ++unwanted;
            }
        }

        // YouTube's own ranking is a weak signal too.
        const double rankBonus = std::max(0.0, 4.0 - 0.5 * static_cast<double>(index));
        if (rankBonus > 0)
            note("search rank #" + std::to_string(index + 1), rankBonus);

        ++index;
        candidates.push_back(std::move(c));
    }

    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const TrackCandidate& a, const TrackCandidate& b) { return a.score > b.score; });
    return candidates;
}

std::optional<TrackMatch> TrackMatcher::find(const TrackQuery& query, const CancellationToken& cancellationToken) const {
    auto results = youtube_.search().getVideos(buildQuery(query), options_.maxSearchResults, cancellationToken);
    auto ranked = rank(query, results);

    if (options_.enableFallbackSearch && (ranked.empty() || ranked.front().score < options_.minimumScore)) {
        std::string alternative = coreTitle(query.title);
        for (const auto& artist : query.artists)
            alternative += " " + artist;
        alternative += " audio";
        auto more = youtube_.search().getVideos(alternative, options_.maxSearchResults, cancellationToken);
        results.insert(results.end(), more.begin(), more.end());
        ranked = rank(query, results);
    }

    if (ranked.empty() || ranked.front().score < options_.minimumScore)
        return std::nullopt;

    TrackMatch match{std::move(ranked.front()), {}};
    match.alternatives.assign(std::make_move_iterator(ranked.begin() + 1), std::make_move_iterator(ranked.end()));
    return match;
}

std::optional<ResolvedTrack> TrackMatcher::resolve(const TrackQuery& query, const std::string& container,
                                                   const CancellationToken& cancellationToken) const {
    auto match = find(query, cancellationToken);
    if (!match)
        return std::nullopt;

    std::vector<TrackCandidate> candidates;
    candidates.push_back(std::move(match->best));
    for (auto& alternative : match->alternatives)
        candidates.push_back(std::move(alternative));

    std::size_t attempts = 0;
    for (std::size_t i = 0; i < candidates.size() && attempts < options_.maxResolveAttempts; ++i) {
        if (candidates[i].score < options_.minimumScore)
            break;
        ++attempts;
        try {
            auto manifest = youtube_.videos().streams().getManifest(candidates[i].video.id(), cancellationToken);
            std::shared_ptr<const Videos::Streams::IAudioStreamInfo> audio = manifest.tryGetBestAudioOnlyStream(container);
            if (!audio && !container.empty())
                audio = manifest.tryGetBestAudioOnlyStream();
            if (!audio)
                audio = Videos::Streams::tryGetWithHighestBitrate(manifest.getMuxedStreams());
            if (!audio)
                continue;
            ResolvedTrack resolved{std::move(candidates[i]), std::move(audio), {}};
            for (std::size_t j = 0; j < candidates.size(); ++j)
                if (j != i)
                    resolved.alternatives.push_back(std::move(candidates[j]));
            return resolved;
        } catch (const Exceptions::RequestLimitExceededException&) {
            throw;  // retrying other candidates would only make it worse
        } catch (const Exceptions::YoutubeExplodeException&) {
            // Region-locked, removed, age-restricted, cipher/player problems, a flaky request...:
            // try the next candidate.
        }
    }
    return std::nullopt;
}

} // namespace YoutubeExplode::Music
