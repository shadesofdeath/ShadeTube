#include "app/Source.h"

#include "app/AppContext.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "musicbrainz/MusicBrainz.h"
#include "spotify/Session.h"

namespace st::app::source {

bool isSpotifyId(const std::string& id) { return id.rfind("spotify:", 0) == 0; }

bool loggedIn() { return ctx().session && ctx().session->loggedIn(); }

spotify::Api* activeApi() { return loggedIn() ? &ctx().session->api() : nullptr; }

std::wstring sourceLabel() {
    if (auto* s = ctx().session; s && s->loggedIn()) {
        std::wstring name = toWide(s->profile().name.empty() ? s->profile().username : s->profile().name);
        return name.empty() ? L"SPOTIFY" : L"SPOTIFY · " + toUpperTr(name);
    }
    // Upper-cased before the brand goes in: Turkish casing would turn "MusicBrainz" into "MUSİCBRAİNZ".
    return i18n::format(toUpperTr(tr(L"Açık katalog · {}")), {L"MUSICBRAINZ"});
}

catalog::SearchResults search(spotify::Api* api, const std::string& query, int limit, const CT& ct) {
    if (api) return api->search(query, limit, ct);
    return mb::search(query, limit, ct);
}

catalog::Album album(spotify::Api* api, const std::string& id, const CT& ct) {
    if (api && isSpotifyId(id)) return api->album(id, ct);
    return mb::album(id, ct);
}

catalog::ArtistPage artistPage(spotify::Api* api, const std::string& id, const CT& ct) {
    if (api && isSpotifyId(id)) return api->artistPage(id, ct);
    return mb::artistPage(id, ct);
}

} // namespace st::app::source
