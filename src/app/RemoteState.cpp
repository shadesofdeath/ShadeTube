#include "app/RemoteState.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace st::app::remote {

using json = nlohmann::json;

namespace {

// Invalid UTF-8 in a tag must not make the whole answer fail.
std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

json artistRefs(const std::vector<catalog::ArtistRef>& artists) {
    json a = json::array();
    for (const auto& r : artists) a.push_back({{"id", r.id}, {"name", r.name}});
    return a;
}

std::string artistNames(const std::vector<catalog::ArtistRef>& artists) {
    std::string s;
    for (const auto& r : artists) {
        if (r.name.empty()) continue;
        if (!s.empty()) s += ", ";
        s += r.name;
    }
    return s;
}

} // namespace

std::string coverUrl(const std::vector<catalog::Image>& images, int minWidth, const CoverMapper& covers) {
    const catalog::Image* img = catalog::pickImage(images, minWidth);
    if (!img || img->url.empty()) return {};
    if (img->url.rfind("https://", 0) == 0 || img->url.rfind("http://", 0) == 0) return img->url;
    return covers ? covers(img->url) : std::string{};
}

json trackJson(const catalog::Track& t, const CoverMapper& covers, int coverPx) {
    json j = {
        {"id", t.id},
        {"title", t.name},
        {"artists", artistRefs(t.artists)},
        {"artist", artistNames(t.artists)},
        {"album", {{"id", t.album.id}, {"name", t.album.name}}},
        {"cover", coverUrl(t.album.images, coverPx, covers)},
        {"durationMs", t.durationMs},
    };
    if (!t.playable) j["playable"] = false;
    if (t.explicitContent) j["explicit"] = true;
    if (t.recommended) j["rec"] = true;
    return j;
}

json albumJson(const catalog::Album& a, const CoverMapper& covers, int coverPx) {
    return {
        {"id", a.id},
        {"title", a.name},
        {"artist", artistNames(a.artists)},
        {"artists", artistRefs(a.artists)},
        {"year", a.year()},
        {"type", a.primaryType},
        {"cover", coverUrl(a.images, coverPx, covers)},
    };
}

json artistJson(const catalog::Artist& a, const CoverMapper& covers, int coverPx) {
    return {{"id", a.id}, {"name", a.name}, {"cover", coverUrl(a.images, coverPx, covers)}};
}

json playlistJson(const catalog::Playlist& p, const std::string& id, const std::string& count, const CoverMapper& covers,
                  int coverPx) {
    return {
        {"id", id},
        {"title", p.name},
        {"owner", p.ownerName},
        {"count", count},
        {"cover", coverUrl(p.images, coverPx, covers)},
    };
}

std::string stateJson(const NowPlaying& s, const CoverMapper& covers) {
    json j = {
        {"v", 1},
        {"status", s.status},
        {"playing", s.playing},
        {"positionMs", std::max<int64_t>(0, s.positionMs)},
        {"durationMs", std::max<int64_t>(0, s.durationMs)},
        {"speed", std::round(s.speed * 100) / 100},
        {"shuffle", s.shuffle},
        {"repeat", s.repeat},
        {"volume", std::round(std::clamp(s.volume, 0.f, 1.f) * 1000) / 1000},
        {"hasNext", s.hasNext},
        {"context", s.contextName},
    };
    if (!s.accentDark.base.empty() && !s.accentLight.base.empty())
        j["accent"] = {{"dark", {{"base", s.accentDark.base}, {"on", s.accentDark.on}}},
                       {"light", {{"base", s.accentLight.base}, {"on", s.accentLight.on}}}};
    if (s.track) {
        json t = trackJson(*s.track, covers, kCoverPx);
        t["thumb"] = coverUrl(s.track->album.images, kThumbPx, covers);
        j["track"] = std::move(t);
        j["liked"] = s.liked;
        j["canLike"] = s.canLike;
        j["live"] = s.live;
        j["podcast"] = s.podcast;
        j["canSeek"] = !s.live && s.durationMs > 0;
        if (s.live && !s.liveTitle.empty()) j["liveTitle"] = s.liveTitle;
    } else {
        j["track"] = nullptr;
    }
    return dump(j);
}

std::string queueJson(const QueueView& q, const CoverMapper& covers) {
    json items = json::array();
    for (const auto& [index, track] : q.items) {
        if (!track) continue;
        json t = trackJson(*track, covers, kThumbPx);
        t["i"] = index;
        items.push_back(std::move(t));
    }
    json j = {{"current", q.current}, {"items", std::move(items)}, {"more", std::max(0, q.more)}};
    if (q.more > 0 && !q.moreText.empty()) j["moreText"] = q.moreText;
    return dump(j);
}

std::string positionJson(int64_t positionMs, bool playing, std::string_view lyricLine) {
    json j = {{"p", std::max<int64_t>(0, positionMs)}, {"playing", playing}};
    if (!lyricLine.empty()) j["line"] = std::string(lyricLine);
    return dump(j);
}

std::string hexColor(float r, float g, float b) {
    auto c = [](float v) { return static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
    char buf[8];
    std::snprintf(buf, sizeof buf, "#%02X%02X%02X", c(r), c(g), c(b));
    return buf;
}

} // namespace st::app::remote
