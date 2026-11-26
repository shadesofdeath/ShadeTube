#pragma once
// Song credits in the UI (docs/DEVELOPMENT.md, "Song credits"): the "Krediler" dialog of the overlay family (every
// credited person by group, each artist a link to their page, the sources and where the credits come from), its item
// in the track menu, and the labels Now Playing's Hakkında tab shares. The data comes from app/Credits.
#include "catalog/Credits.h"
#include "catalog/Models.h"

#include <optional>
#include <string>
#include <vector>

namespace st::ui {
struct MenuItem;
}

namespace st::app {

// "Krediler" for a song's menu (one song; not for stations or podcast episodes).
std::optional<ui::MenuItem> creditsMenuItem(const catalog::Track& t);
// Opens the "Krediler" dialog for `t`: the cached credits at once, else they are fetched while it is open.
void showCredits(const catalog::Track& t);

std::wstring creditGroupLabel(catalog::CreditGroup g);   // "Seslendirenler", "Yazarlar"...
// A role as the UI says it: the known ones translated ("composer" -> "Besteci"), others (instruments: "piano") as the
// source wrote them with a capital first letter.
std::wstring creditRoleLabel(const std::string& role);
std::wstring creditRolesText(const std::vector<std::string>& roles);   // "Besteci · Söz yazarı"
std::wstring creditsProviderLabel(const std::string& provider);        // "Spotify" / "MusicBrainz"

} // namespace st::app
