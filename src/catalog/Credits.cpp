#include "catalog/Credits.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace st::catalog {

namespace {

bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

std::string_view trimmed(std::string_view s) {
    while (!s.empty() && space(s.front())) s.remove_prefix(1);
    while (!s.empty() && space(s.back())) s.remove_suffix(1);
    return s;
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool sameName(std::string_view a, std::string_view b) {
    a = trimmed(a);
    b = trimmed(b);
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

} // namespace

std::string canonicalRole(std::string_view role) {
    std::string s;
    s.reserve(role.size());
    bool pendingSpace = false;
    for (char c : trimmed(role)) {
        if (space(c) || c == '_') {
            pendingSpace = !s.empty();
            continue;
        }
        if (pendingSpace) s.push_back(' ');
        pendingSpace = false;
        s.push_back(lower(c));
    }
    static const std::unordered_map<std::string, std::string> kSynonyms = {
        {"songwriter", "writer"},
        {"song writer", "writer"},
        {"written by", "writer"},
        {"composed by", "composer"},
        {"lyrics", "lyricist"},
        {"lyrics by", "lyricist"},
        {"mix", "mixing engineer"},
        {"mixer", "mixing engineer"},
        {"mixing", "mixing engineer"},
        {"mix engineer", "mixing engineer"},
        {"mixed by", "mixing engineer"},
        {"mastering", "mastering engineer"},
        {"mastered by", "mastering engineer"},
        {"recording", "recording engineer"},
        {"recorded by", "recording engineer"},
        {"vocal", "vocals"},
        {"vocalist", "vocals"},
        {"lead vocal", "lead vocals"},
        {"lead vocalist", "lead vocals"},
        {"background vocal", "background vocals"},
        {"background vocalist", "background vocals"},
        {"backing vocals", "background vocals"},
        {"backing vocal", "background vocals"},
        {"primary artist", "main artist"},
        {"featuring", "featured artist"},
        {"featured", "featured artist"},
        {"feat.", "featured artist"},
        {"co producer", "co-producer"},
        {"coproducer", "co-producer"},
        {"produced by", "producer"},
        {"programmer", "programming"},
        {"programmed by", "programming"},
        {"arrangement", "arranger"},
        {"arranged by", "arranger"},
        {"remix", "remixer"},
        {"remixed by", "remixer"},
    };
    if (const auto it = kSynonyms.find(s); it != kSynonyms.end()) return it->second;
    return s;
}

CreditGroup roleGroup(std::string_view role, std::string_view hint) {
    const std::string r = canonicalRole(role);
    static const std::unordered_set<std::string> kPerformers = {
        "main artist", "featured artist", "performer", "vocals", "lead vocals", "background vocals", "choir",
        "orchestra", "conductor", "band", "ensemble", "rapper", "soloist", "musician", "guitar", "bass guitar", "bass",
        "drums", "piano", "keyboard", "keyboards", "synthesizer", "violin", "cello", "viola", "saxophone", "trumpet",
        "trombone", "percussion", "strings", "organ", "flute", "harp", "clarinet", "horn", "electric guitar",
        "acoustic guitar", "double bass", "drum machine"};
    static const std::unordered_set<std::string> kWriters = {"composer", "lyricist", "writer", "author", "librettist",
                                                             "translator", "arranger", "adapter", "poet"};
    static const std::unordered_set<std::string> kProducers = {"producer", "co-producer", "executive producer",
                                                               "additional producer", "vocal producer",
                                                               "assistant producer", "programming", "remixer"};
    if (!r.empty()) {
        if (kPerformers.contains(r)) return CreditGroup::Performers;
        if (kWriters.contains(r)) return CreditGroup::Writers;
        if (kProducers.contains(r)) return CreditGroup::Producers;
        if (contains(r, "engineer") || r == "editor") return CreditGroup::Engineers;
        if (contains(r, "producer")) return CreditGroup::Producers;
        if (contains(r, "vocal")) return CreditGroup::Performers;
        if (contains(r, "composer") || contains(r, "writer") || contains(r, "lyric")) return CreditGroup::Writers;
    }
    const std::string h = canonicalRole(hint);
    if (!h.empty()) {
        if (contains(h, "perform") || contains(h, "artist") || contains(h, "vocal") || contains(h, "musician"))
            return CreditGroup::Performers;
        if (contains(h, "writ") || contains(h, "compos") || contains(h, "lyric") || contains(h, "song"))
            return CreditGroup::Writers;
        if (contains(h, "produc")) return CreditGroup::Producers;
        if (contains(h, "engineer") || contains(h, "mix") || contains(h, "master")) return CreditGroup::Engineers;
    }
    return CreditGroup::Other;
}

bool Credits::empty() const {
    return std::all_of(groups.begin(), groups.end(), [](const auto& g) { return g.empty(); });
}

size_t Credits::people() const {
    size_t n = 0;
    for (const auto& g : groups) n += g.size();
    return n;
}

void Credits::add(std::string_view name, std::string_view artistId, std::string_view role, std::optional<CreditGroup> group) {
    name = trimmed(name);
    if (name.empty()) return;
    const std::string r = canonicalRole(role);
    auto& list = groups[static_cast<size_t>(group.value_or(roleGroup(r)))];
    auto it = std::find_if(list.begin(), list.end(), [&](const Credit& c) { return sameName(c.name, name); });
    if (it == list.end()) {
        list.push_back({std::string(name), std::string(trimmed(artistId)), {}});
        it = std::prev(list.end());
    } else if (it->artistId.empty()) {
        it->artistId = std::string(trimmed(artistId));
    }
    if (!r.empty() && std::find(it->roles.begin(), it->roles.end(), r) == it->roles.end()) it->roles.push_back(r);
}

void Credits::addSource(std::string_view name) {
    name = trimmed(name);
    if (name.empty()) return;
    if (std::none_of(sources.begin(), sources.end(), [&](const std::string& s) { return sameName(s, name); }))
        sources.emplace_back(name);
}

} // namespace st::catalog
