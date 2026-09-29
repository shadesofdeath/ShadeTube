#include "app/SmartMix.h"

#include "core/Utf.h"

#include <windows.h>

#include <algorithm>
#include <random>

namespace st::app::smartmix {

namespace {

// Lower case, letters and digits only (any script); the text stops at a " - " suffix and skips bracketed parts.
std::string fold(const std::string& utf8, bool cutSuffix) {
    std::wstring w = toWide(utf8);
    if (!w.empty()) CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));   // Unicode, not the C locale
    if (cutSuffix) {
        if (const size_t dash = w.find(L" - "); dash != std::wstring::npos && dash > 0) w.resize(dash);
    }
    std::wstring out;
    int depth = 0;
    for (wchar_t c : w) {
        if (c == L'(' || c == L'[') {
            ++depth;
            continue;
        }
        if (c == L')' || c == L']') {
            depth = std::max(0, depth - 1);
            continue;
        }
        if (depth > 0) continue;
        if (c == L'ı' || c == L'İ') c = L'i';   // Turkish dotless / dotted I: "ISTANBUL" == "İstanbul" == "istanbul"
        if (IsCharAlphaNumericW(c)) out.push_back(c);
    }
    return toUtf8(out);
}

std::string firstArtist(const catalog::Track& t) {
    for (const auto& a : t.artists)
        if (!a.name.empty()) return fold(a.name, false);
    return {};
}

} // namespace

uint64_t hash(const std::string& s, uint64_t basis) {
    uint64_t h = basis;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string foldName(const std::string& name) { return fold(name, false); }

std::string trackKey(const catalog::Track& t) {
    const std::string title = fold(t.name, true);
    if (title.empty()) return {};
    return title + "|" + firstArtist(t);
}

void Exclusions::add(const catalog::Track& t) {
    if (!t.id.empty()) ids.insert(t.id);
    if (std::string k = trackKey(t); !k.empty()) keys.insert(std::move(k));
}

bool Exclusions::contains(const catalog::Track& t) const {
    if (!t.id.empty() && ids.contains(t.id)) return true;
    const std::string k = trackKey(t);
    return !k.empty() && keys.contains(k);
}

std::vector<catalog::Track> candidates(const std::vector<catalog::Track>& pool, const Exclusions& exclude,
                                       const std::function<bool(const catalog::Track&)>& blocked, uint64_t seed) {
    std::vector<catalog::Track> usable;
    Exclusions seen;
    for (const auto& t : pool) {
        if (!t.playable || t.name.empty() || t.id.empty()) continue;
        if (exclude.contains(t) || seen.contains(t)) continue;
        if (blocked && blocked(t)) continue;
        seen.add(t);
        usable.push_back(t);
    }
    std::mt19937_64 rng(seed);
    std::shuffle(usable.begin(), usable.end(), rng);
    // Spread the artists: take the next song whose first artist differs from the last one taken, when there is one.
    std::vector<catalog::Track> out;
    out.reserve(usable.size());
    std::string last;
    while (!usable.empty()) {
        auto it = std::find_if(usable.begin(), usable.end(), [&](const catalog::Track& t) { return firstArtist(t) != last; });
        if (it == usable.end()) it = usable.begin();
        last = firstArtist(*it);
        out.push_back(std::move(*it));
        usable.erase(it);
    }
    for (auto& t : out) t.recommended = true;
    return out;
}

std::vector<int> insertionPoints(const std::vector<bool>& isRec, int pos, uint64_t seed, int window) {
    std::vector<int> points;
    const int n = static_cast<int>(isRec.size());
    if (pos < 0 || pos >= n) return points;
    std::mt19937_64 rng(seed);
    auto drawGap = [&] { return kMinGap + static_cast<int>(rng() % (kMaxGap - kMinGap + 1)); };
    // Own songs since the last recommendation, up to and including the playing one.
    int run = 0;
    for (int i = pos; i >= 0 && !isRec[i] && run < kMaxGap; --i) ++run;
    int target = drawGap();
    const int end = std::min(n, pos + 1 + std::max(0, window));
    for (int i = pos + 1; i < end; ++i) {
        if (isRec[i]) {
            run = 0;
            target = drawGap();
            continue;
        }
        ++run;
        if (run < target) continue;
        // A recommendation already coming soon enough keeps this stretch as it is.
        int ownAhead = 0;
        bool recAhead = false;
        for (int j = i + 1; j < n && ownAhead <= kMaxGap; ++j) {
            if (isRec[j]) {
                recAhead = true;
                break;
            }
            ++ownAhead;
        }
        if (recAhead && run + ownAhead <= kMaxGap) continue;
        points.push_back(i + 1);   // after this song (i + 1 >= pos + 2: never right after the playing one)
        run = 0;
        target = drawGap();
    }
    return points;
}

std::vector<catalog::Track> interleave(const std::vector<catalog::Track>& own, const std::vector<catalog::Track>& recs,
                                       uint64_t seed) {
    std::vector<catalog::Track> out;
    out.reserve(own.size() + recs.size());
    std::mt19937_64 rng(seed);
    auto drawGap = [&] { return kMinGap + static_cast<int>(rng() % (kMaxGap - kMinGap + 1)); };
    size_t next = 0;
    int run = 0, target = drawGap();
    for (const auto& t : own) {
        out.push_back(t);
        if (++run < target || next >= recs.size()) continue;
        catalog::Track r = recs[next++];
        r.recommended = true;
        out.push_back(std::move(r));
        run = 0;
        target = drawGap();
    }
    return out;
}

} // namespace st::app::smartmix
