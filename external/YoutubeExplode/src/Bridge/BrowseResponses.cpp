#include "BrowseResponses.hpp"

#include "../Utils/StringUtils.hpp"

namespace YoutubeExplode::detail {

namespace {

const Json* firstRun(const Json* text) { return dig(text, {"runs", 0}); }

std::optional<std::string> browseId(const Json* node) {
    return jsonString(dig(node, {"navigationEndpoint", "browseEndpoint", "browseId"}));
}

std::optional<int> parseCount(const std::optional<std::string>& text) {
    if (!text)
        return std::nullopt;
    auto first = substringUntil(trim(*text), " ");
    auto digits = stripNonDigits(first);
    if (digits.empty() || digits.size() != replaceAll(replaceAll(first, ",", ""), ".", "").size())
        return std::nullopt;
    return tryParseInt(digits);
}

} // namespace

// ---- Search ---------------------------------------------------------------------------------------

SearchResponse SearchResponse::parse(const std::string& raw) {
    const auto json = parseJson(raw);
    SearchResponse response;

    // The search response is incredibly inconsistent (at least 5 layouts), so rely on
    // descendant searches, which are slower but resilient.
    const Json* root = dig(json, {"contents"});
    if (!root)
        root = dig(json, {"onResponseReceivedCommands"});
    if (!root)
        return response;

    for (const auto* v : findDescendants(*root, "videoRenderer")) {
        SearchVideoData d;
        d.id = jsonString(dig(v, {"videoId"}));
        d.title = jsonText(dig(v, {"title"}));
        const Json* authorRun = firstRun(dig(v, {"longBylineText"}));
        if (!authorRun)
            authorRun = firstRun(dig(v, {"shortBylineText"}));
        d.author = jsonString(dig(authorRun, {"text"}));
        d.channelId = browseId(authorRun);
        if (!d.channelId)
            d.channelId = browseId(dig(v, {"channelThumbnailSupportedRenderers", "channelThumbnailWithLinkRenderer"}));
        d.duration = parseClockText(jsonText(dig(v, {"lengthText"})));
        d.thumbnails = parseThumbnails(dig(v, {"thumbnail"}));
        response.videos.push_back(std::move(d));
    }

    for (const auto* p : findDescendants(*root, "lockupViewModel")) {
        const auto contentType = jsonString(dig(p, {"contentType"})).value_or("");
        if (!contentType.empty() && !contains(contentType, "PLAYLIST"))
            continue;
        SearchPlaylistData d;
        d.id = jsonString(dig(p, {"contentId"}));
        const Json* metadata = dig(p, {"metadata", "lockupMetadataViewModel"});
        d.title = jsonString(dig(metadata, {"title", "content"}));
        if (metadata) {
            if (const auto* parts = findFirstDescendant(*metadata, "metadataParts")) {
                const Json* text = dig(parts, {0, "text"});
                d.author = jsonString(dig(text, {"content"}));
                d.channelId =
                    jsonString(dig(text, {"commandRuns", 0, "onTap", "innertubeCommand", "browseEndpoint", "browseId"}));
            }
        }
        d.thumbnails = parseThumbnails(dig(p, {"contentImage", "collectionThumbnailViewModel", "primaryThumbnail",
                                               "thumbnailViewModel", "image", "sources"}));
        if (d.thumbnails.empty())
            d.thumbnails = parseThumbnails(dig(p, {"contentImage", "thumbnailViewModel", "image", "sources"}));
        response.playlists.push_back(std::move(d));
    }

    for (const auto* p : findDescendants(*root, "playlistRenderer")) {
        SearchPlaylistData d;
        d.id = jsonString(dig(p, {"playlistId"}));
        d.title = jsonText(dig(p, {"title"}));
        const Json* authorRun = firstRun(dig(p, {"longBylineText"}));
        d.author = jsonString(dig(authorRun, {"text"}));
        d.channelId = browseId(authorRun);
        if (const auto* thumbs = dig(p, {"thumbnails"}))
            for (const auto* t : findDescendants(*thumbs, "thumbnails")) {
                auto parsed = parseThumbnails(t);
                d.thumbnails.insert(d.thumbnails.end(), parsed.begin(), parsed.end());
            }
        response.playlists.push_back(std::move(d));
    }

    for (const auto* c : findDescendants(*root, "channelRenderer")) {
        SearchChannelData d;
        d.id = jsonString(dig(c, {"channelId"}));
        d.title = jsonText(dig(c, {"title"}));
        d.thumbnails = parseThumbnails(dig(c, {"thumbnail"}));
        response.channels.push_back(std::move(d));
    }

    if (const auto* command = findFirstDescendant(*root, "continuationCommand"))
        response.continuationToken = jsonString(dig(command, {"token"}));
    return response;
}

// ---- Playlists ------------------------------------------------------------------------------------

PlaylistData parsePlaylistBrowseResponse(const std::string& raw) {
    const auto json = parseJson(raw);
    PlaylistData d;
    const Json* sidebar = dig(json, {"sidebar", "playlistSidebarRenderer", "items"});
    d.isAvailable = sidebar != nullptr;
    if (!sidebar)
        return d;
    const Json* primary = dig(sidebar, {0, "playlistSidebarPrimaryInfoRenderer"});
    const Json* secondary = dig(sidebar, {1, "playlistSidebarSecondaryInfoRenderer"});

    d.title = jsonText(dig(primary, {"title"}));
    if (!d.title)
        d.title = jsonString(dig(primary, {"titleForm", "inlineFormRenderer", "formField", "textInputFormFieldRenderer", "value"}));

    const Json* owner = dig(secondary, {"videoOwner", "videoOwnerRenderer"});
    d.author = jsonText(dig(owner, {"title"}));
    d.channelId = browseId(owner);
    if (!d.channelId)
        d.channelId = browseId(firstRun(dig(owner, {"title"})));

    d.description = jsonText(dig(primary, {"description"}));
    if (!d.description)
        d.description = jsonString(
            dig(primary, {"descriptionForm", "inlineFormRenderer", "formField", "textInputFormFieldRenderer", "value"}));

    d.count = parseCount(jsonText(dig(primary, {"stats", 0})));

    d.thumbnails = parseThumbnails(dig(primary, {"thumbnailRenderer", "playlistVideoThumbnailRenderer", "thumbnail"}));
    if (d.thumbnails.empty())
        d.thumbnails = parseThumbnails(dig(primary, {"thumbnailRenderer", "playlistCustomThumbnailRenderer", "thumbnail"}));
    return d;
}

PlaylistNextResponse PlaylistNextResponse::parse(const std::string& raw) {
    const auto json = parseJson(raw);
    PlaylistNextResponse r;
    const Json* root = dig(json, {"contents", "twoColumnWatchNextResults", "playlist", "playlist"});
    r.playlist.isAvailable = root != nullptr;
    r.visitorData = jsonString(dig(json, {"responseContext", "visitorData"}));
    if (!root)
        return r;

    r.playlist.title = jsonString(dig(root, {"title"}));
    r.playlist.author = jsonText(dig(root, {"ownerName"}));
    r.playlist.count = parseCount(jsonString(dig(root, {"totalVideosText", "runs", 0, "text"})));
    if (!r.playlist.count)
        r.playlist.count = parseCount(jsonString(dig(root, {"videoCountText", "runs", 2, "text"})));

    if (const auto* contents = dig(root, {"contents"}); contents && contents->is_array()) {
        for (const auto& item : *contents) {
            const Json* v = dig(item, {"playlistPanelVideoRenderer"});
            if (!v)
                continue;
            PlaylistVideoData d;
            d.index = jsonInt(dig(v, {"navigationEndpoint", "watchEndpoint", "index"}));
            d.id = jsonString(dig(v, {"videoId"}));
            d.title = jsonText(dig(v, {"title"}));
            const Json* authorRun = firstRun(dig(v, {"longBylineText"}));
            if (!authorRun)
                authorRun = firstRun(dig(v, {"shortBylineText"}));
            d.author = jsonString(dig(authorRun, {"text"}));
            d.channelId = browseId(authorRun);
            if (!d.channelId) {
                // Videos with multiple authors: take the uploader (first entry of the dialog).
                d.channelId = jsonString(dig(
                    authorRun, {"navigationEndpoint", "showDialogCommand", "panelLoadingStrategy", "inlineContent",
                                "dialogViewModel", "customContent", "listViewModel", "listItems", 0,
                                "listItemViewModel", "rendererContext", "commandContext", "onTap",
                                "innertubeCommand", "browseEndpoint", "browseId"}));
            }
            if (auto seconds = jsonInt64(dig(v, {"lengthSeconds"})))
                d.duration = std::chrono::duration_cast<TimeSpan>(std::chrono::seconds(*seconds));
            else
                d.duration = parseClockText(jsonText(dig(v, {"lengthText"})));
            d.thumbnails = parseThumbnails(dig(v, {"thumbnail"}));
            r.videos.push_back(std::move(d));
        }
    }
    if (!r.videos.empty())
        r.playlist.thumbnails = r.videos.front().thumbnails;
    return r;
}

} // namespace YoutubeExplode::detail
