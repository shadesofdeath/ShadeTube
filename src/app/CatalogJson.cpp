#include "app/CatalogJson.h"

namespace st::app {

using json = nlohmann::json;
using namespace catalog;

static json imagesToJson(const std::vector<Image>& images) {
    json a = json::array();
    for (const auto& i : images) a.push_back({{"u", i.url}, {"w", i.width}, {"h", i.height}});
    return a;
}

static std::vector<Image> imagesFromJson(const json& j) {
    std::vector<Image> out;
    if (!j.is_array()) return out;
    for (const auto& i : j) out.push_back({i.value("u", ""), i.value("w", 0), i.value("h", 0)});
    return out;
}

static json artistsToJson(const std::vector<ArtistRef>& a) {
    json arr = json::array();
    for (const auto& r : a) arr.push_back({{"id", r.id}, {"n", r.name}});
    return arr;
}

static std::vector<ArtistRef> artistsFromJson(const json& j) {
    std::vector<ArtistRef> out;
    if (!j.is_array()) return out;
    for (const auto& r : j) out.push_back({r.value("id", ""), r.value("n", "")});
    return out;
}

json toJson(const Track& t) {
    return {{"id", t.id},   {"v", t.videoId},      {"n", t.name},       {"a", artistsToJson(t.artists)},
            {"al", {{"id", t.album.id}, {"n", t.album.name}, {"img", imagesToJson(t.album.images)}}},
            {"d", t.durationMs}, {"e", t.explicitContent}, {"t", t.addedAt}, {"no", t.trackNumber}};
}

Track trackFromJson(const json& j) {
    Track t;
    if (!j.is_object()) return t;
    t.id = j.value("id", "");
    t.videoId = j.value("v", "");
    t.name = j.value("n", "");
    t.artists = artistsFromJson(j.value("a", json::array()));
    if (auto al = j.find("al"); al != j.end() && al->is_object()) {
        t.album.id = al->value("id", "");
        t.album.name = al->value("n", "");
        t.album.images = imagesFromJson(al->value("img", json::array()));
    }
    t.durationMs = j.value("d", 0);
    t.explicitContent = j.value("e", false);
    t.addedAt = j.value("t", int64_t{0});
    t.trackNumber = j.value("no", 0);
    return t;
}

json toJson(const Album& a) {
    return {{"id", a.id}, {"n", a.name}, {"pt", a.primaryType}, {"a", artistsToJson(a.artists)},
            {"img", imagesToJson(a.images)}, {"date", a.firstReleaseDate}, {"tt", a.totalTracks}};
}

Album albumFromJson(const json& j) {
    Album a;
    if (!j.is_object()) return a;
    a.id = j.value("id", "");
    a.name = j.value("n", "");
    a.primaryType = j.value("pt", "");
    a.artists = artistsFromJson(j.value("a", json::array()));
    a.images = imagesFromJson(j.value("img", json::array()));
    a.firstReleaseDate = j.value("date", "");
    a.totalTracks = j.value("tt", 0);
    return a;
}

json toJson(const Artist& a) {
    return {{"id", a.id}, {"n", a.name}, {"img", imagesToJson(a.images)}, {"c", a.country}};
}

Artist artistFromJson(const json& j) {
    Artist a;
    if (!j.is_object()) return a;
    a.id = j.value("id", "");
    a.name = j.value("n", "");
    a.images = imagesFromJson(j.value("img", json::array()));
    a.country = j.value("c", "");
    return a;
}

} // namespace st::app
