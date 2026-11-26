#pragma once
// Song credits: who made a song - the performers, the writers (composers, lyricists), the producers, the engineers and
// everyone else credited - and the sources (labels / distributors) that delivered it, like Spotify's "Credits". Filled
// by the Spotify client (the web player's credits) and the MusicBrainz client (recording and work relationships);
// app/Credits caches them per track and shows them (Now Playing's Hakkında, the "Krediler" dialog).
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::catalog {

enum class CreditGroup { Performers, Writers, Producers, Engineers, Other };
inline constexpr size_t kCreditGroups = 5;

struct Credit {
    std::string name;
    std::string artistId;            // spotify:artist: URI or artist MBID when the person has an artist page, else ""
    std::vector<std::string> roles;  // canonicalRole() forms ("composer", "producer", "piano"...), in credit order
};

struct Credits {
    std::string provider;            // "spotify" | "musicbrainz" ("" = nothing known)
    std::array<std::vector<Credit>, kCreditGroups> groups;   // by CreditGroup, people in credit order
    std::vector<std::string> sources;   // labels / distributors ("Columbia", "Sony Music Entertainment")

    bool empty() const;              // nobody credited (sources alone don't count)
    size_t people() const;           // entries over all groups (a person in two groups counts twice)
    const std::vector<Credit>& group(CreditGroup g) const { return groups[static_cast<size_t>(g)]; }
    // One credited role. It goes to the role's group (or `group` when the source says so) and is merged into that
    // group's entry of the same person (same name, ASCII case ignored): roles are added once, an artist id the entry
    // lacks is taken over. An empty role only lists the person. Blank names are ignored.
    void add(std::string_view name, std::string_view artistId, std::string_view role,
             std::optional<CreditGroup> group = std::nullopt);
    void addSource(std::string_view name);   // once per name, blank ignored
};

// The canonical form of a role as the sources write it ("Composer", " Lead Vocals", "Mixing Engineer", "mix"): lower
// case, trimmed, inner whitespace collapsed, known synonyms folded ("songwriter" -> "writer", "mix" -> "mixing
// engineer", "vocal" -> "vocals", "main artist" / "primary artist" -> "main artist").
std::string canonicalRole(std::string_view role);
// The group a role belongs to. Roles it doesn't know go by `hint` (the source's own group title: "Performers",
// "Writers", "Composition & Lyrics", "Production & Engineering"...), else Other.
CreditGroup roleGroup(std::string_view role, std::string_view hint = {});

} // namespace st::catalog
