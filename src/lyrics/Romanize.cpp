#include "lyrics/Romanize.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/Utf.h"

#include <nlohmann/json.hpp>
#include <windows.h>
#include <unknwn.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <thread>

namespace st::lyrics {

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

constexpr int kCacheVersion = 2;   // bump when a rule changes: older answers are made again

// ---- Character classes -----------------------------------------------------------------------------------------------

bool isHangul(wchar_t c) { return c >= 0xAC00 && c <= 0xD7A3; }
bool isHiragana(wchar_t c) { return c >= 0x3041 && c <= 0x3096; }
bool isKatakana(wchar_t c) { return (c >= 0x30A1 && c <= 0x30FA) || (c >= 0x31F0 && c <= 0x31FF); }
bool isKanaMark(wchar_t c) { return c == 0x30FC || c == 0xFF70; }   // ー, halfwidth ｰ
bool isKana(wchar_t c) { return isHiragana(c) || isKatakana(c) || isKanaMark(c); }
bool isHan(wchar_t c) { return (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0x3400 && c <= 0x4DBF) || (c >= 0xF900 && c <= 0xFAFF) || c == 0x3005 || c == 0x3006; }
bool isCyrillic(wchar_t c) { return c >= 0x0400 && c <= 0x04FF; }
bool isSpace(wchar_t c) { return c == L' ' || c == L'\t' || c == 0x3000; }

// ---- Korean: Revised Romanization ---------------------------------------------------------------------------------

// Initial consonants (choseong) 0..18 and finals (jongseong) 1..27 as Unicode orders them.
enum Ini { G, KK, N, D, TT, R, M, B, PP, S, SS, IEUNG, J, JJ, CH, K, T, P, H };
const char* const kIni[] = {"g", "kk", "n", "d", "tt", "r", "m", "b", "pp", "s", "ss", "", "j", "jj", "ch", "k", "t", "p", "h"};
const char* const kVowel[] = {"a", "ae", "ya", "yae", "eo", "e", "yeo", "ye", "o", "wa", "wae",
                              "oe", "yo", "u", "wo", "we", "wi", "yu", "eu", "ui", "i"};
constexpr int kVowelI = 20;   // ㅣ
enum Fin { F_NONE, F_G, F_KK, F_GS, F_N, F_NJ, F_NH, F_D, F_L, F_LG, F_LM, F_LB, F_LS, F_LT, F_LP, F_LH, F_M, F_B, F_BS,
           F_S, F_SS, F_NG, F_J, F_CH, F_K, F_T, F_P, F_H };
// The sound a final is pronounced as before a consonant or at the end (neutralization).
enum Snd { S_NONE, S_K, S_N, S_T, S_L, S_M, S_P, S_NG };
const char* const kSnd[] = {"", "k", "n", "t", "l", "m", "p", "ng"};

// A single final moved in front of the next syllable's vowel (liaison): the initial it becomes; -1 = none (ㅇ stays,
// ㅎ drops).
int finalAsInitial(int f) {
    switch (f) {
    case F_G: return G;
    case F_KK: return KK;
    case F_N: return N;
    case F_D: return D;
    case F_L: return R;
    case F_M: return M;
    case F_B: return B;
    case F_S: return S;
    case F_SS: return SS;
    case F_J: return J;
    case F_CH: return CH;
    case F_K: return K;
    case F_T: return T;
    case F_P: return P;
    default: return -1;
    }
}

// A double final: what stays and what moves on (or is heard before ㅎ).
bool splitCluster(int f, int& first, int& second) {
    switch (f) {
    case F_GS: first = F_G; second = S; return true;
    case F_NJ: first = F_N; second = J; return true;
    case F_NH: first = F_N; second = H; return true;
    case F_LG: first = F_L; second = G; return true;
    case F_LM: first = F_L; second = M; return true;
    case F_LB: first = F_L; second = B; return true;
    case F_LS: first = F_L; second = S; return true;
    case F_LT: first = F_L; second = T; return true;
    case F_LP: first = F_L; second = P; return true;
    case F_LH: first = F_L; second = H; return true;
    case F_BS: first = F_B; second = S; return true;
    default: return false;
    }
}

Snd neutral(int f, int nextInitial) {
    switch (f) {
    case F_G: case F_KK: case F_K: case F_GS: return S_K;
    case F_LG: return nextInitial == G ? S_L : S_K;   // 맑게 malge, 읽다 ikda
    case F_N: case F_NJ: case F_NH: return S_N;
    case F_D: case F_S: case F_SS: case F_J: case F_CH: case F_T: case F_H: return S_T;
    case F_L: case F_LB: case F_LS: case F_LT: case F_LH: return S_L;
    case F_M: case F_LM: return S_M;
    case F_B: case F_P: case F_BS: case F_LP: return S_P;
    case F_NG: return S_NG;
    default: return S_NONE;
    }
}

struct Syl {
    int ini, vowel, fin;
};

std::string romanizeHangulWord(const std::vector<Syl>& in) {
    std::vector<Syl> s = in;
    const size_t n = s.size();
    // Changes across each syllable boundary on the letters: liaison, ㅎ, palatalization.
    for (size_t i = 0; i + 1 < n; ++i) {
        int& f = s[i].fin;
        int& next = s[i + 1].ini;
        const bool toI = s[i + 1].vowel == kVowelI;
        if (f == F_NONE) continue;
        int first = 0, second = 0;
        if (next == IEUNG) {   // a vowel follows: the final is heard at its start
            if (f == F_NG) continue;
            if (f == F_H) {
                f = F_NONE;   // 좋아 joa
            } else if (splitCluster(f, first, second)) {
                f = first;
                if (second != H) next = second;   // 읽어 ilgeo, 없어 eopseo; 괜찮아 gwaenchana
            } else {
                next = finalAsInitial(f);
                f = F_NONE;
            }
            if (toI && next == D) next = J;        // 굳이 guji
            else if (toI && next == T) next = CH;  // 같이 gachi
            continue;
        }
        if (next == H) {   // ㄱ ㄷ ㅂ ㅈ + ㅎ: aspirated (축하 chuka, 입학 ipak, 못해 motae)
            int keep = F_NONE, f2 = f;
            if (splitCluster(f, first, second) && second != H) {
                keep = first;
                f2 = second == G ? F_G : second == B ? F_B : second == J ? F_J : F_NONE;
            }
            switch (f2) {
            case F_G: case F_KK: case F_K: next = K; f = keep; break;
            case F_B: case F_P: next = P; f = keep; break;
            case F_J: next = CH; f = keep; break;
            case F_D: case F_S: case F_SS: case F_CH: case F_T: next = toI ? CH : T; f = keep; break;   // 닫히다 dachida
            case F_H: f = F_NONE; break;
            default:
                if (f == F_NH) f = F_N;
                else if (f == F_LH) f = F_L;
                break;
            }
            continue;
        }
        if (f == F_H || f == F_NH || f == F_LH) {   // ㅎ before a consonant
            const int rest = f == F_NH ? F_N : f == F_LH ? F_L : F_NONE;
            if (next == G) next = K, f = rest;          // 좋고 joko, 않고 anko
            else if (next == D) next = T, f = rest;     // 놓다 nota, 많다 manta
            else if (next == J) next = CH, f = rest;    // 좋지 jochi
            else if (next == S) f = rest;               // 좋습니다 joseumnida
            else if (next == N) f = rest == F_NONE ? F_N : rest;   // 놓는 nonneun, 않는 anneun, 뚫는 ttulleun
        }
    }
    // Sounds of the finals, then the assimilations between a final sound and the next initial.
    std::vector<Snd> snd(n);
    for (size_t i = 0; i < n; ++i) snd[i] = neutral(s[i].fin, i + 1 < n ? s[i + 1].ini : -1);
    for (size_t i = 0; i + 1 < n; ++i) {
        Snd& f = snd[i];
        int& next = s[i + 1].ini;
        if (next == R) {
            if (f == S_N) f = S_L;                                 // 신라 silla
            else if (f == S_M || f == S_NG) next = N;              // 종로 jongno, 심리 simni
            else if (f == S_K || f == S_P || f == S_T) next = N;   // 독립 dongnip, 협력 hyeomnyeok
        } else if (next == N && f == S_L) {
            next = R;                                              // 설날 seollal (the ㄹ reads l after l)
        }
        if (next == N || next == M) {                              // nasalization: 국물 gungmul, 합니다 hamnida
            if (f == S_K) f = S_NG;
            else if (f == S_T) f = S_N;
            else if (f == S_P) f = S_M;
        }
    }
    std::string out;
    for (size_t i = 0; i < n; ++i) {
        const bool lAfterL = s[i].ini == R && i > 0 && snd[i - 1] == S_L;
        out += lAfterL ? "l" : kIni[s[i].ini];
        out += kVowel[s[i].vowel];
        out += kSnd[snd[i]];
    }
    return out;
}

// ---- Japanese: kana to modified Hepburn -------------------------------------------------------------------------

// Hiragana U+3041..U+3096 (katakana map onto it); small kana are their vowel / y-sound, っ is handled apart.
const char* const kKana[] = {
    "a",  "a",  "i",  "i",   "u",  "u",  "e",  "e",  "o",  "o",                                      // ぁ..お
    "ka", "ga", "ki", "gi",  "ku", "gu", "ke", "ge", "ko", "go",                                     // か..ご
    "sa", "za", "shi", "ji", "su", "zu", "se", "ze", "so", "zo",                                     // さ..ぞ
    "ta", "da", "chi", "ji", "",   "tsu", "zu", "te", "de", "to", "do",                              // た..ど (っ)
    "na", "ni", "nu", "ne",  "no",                                                                   // な..の
    "ha", "ba", "pa", "hi",  "bi", "pi", "fu", "bu", "pu", "he", "be", "pe", "ho", "bo", "po",      // は..ぽ
    "ma", "mi", "mu", "me",  "mo",                                                                   // ま..も
    "ya", "ya", "yu", "yu",  "yo", "yo",                                                             // ゃ..よ
    "ra", "ri", "ru", "re",  "ro",                                                                   // ら..ろ
    "wa", "wa", "i",  "e",   "o",  "n",  "vu", "ka", "ke",                                           // ゎ..ゖ
};
static_assert(std::size(kKana) == 0x3096 - 0x3041 + 1);

wchar_t toHiragana(wchar_t c) {
    if (c >= 0x30A1 && c <= 0x30F6) return static_cast<wchar_t>(c - 0x60);
    return c;
}
bool isSmallY(wchar_t h) { return h == 0x3083 || h == 0x3085 || h == 0x3087; }                       // ゃゅょ
bool isSmallVowel(wchar_t h) { return h == 0x3041 || h == 0x3043 || h == 0x3045 || h == 0x3047 || h == 0x3049; }   // ぁぃぅぇぉ
bool isVowelChar(char c) { return c == 'a' || c == 'i' || c == 'u' || c == 'e' || c == 'o'; }

// Japanese punctuation and fullwidth forms in Latin text.
const wchar_t* punctuation(wchar_t c) {
    switch (c) {
    case 0x3001: return L", ";   // 、
    case 0x3002: return L". ";   // 。
    case 0x300C: case 0x300E: case 0x201C: return L"\"";
    case 0x300D: case 0x300F: case 0x201D: return L"\"";
    case 0x30FB: return L" ";     // ・
    case 0x3000: return L" ";
    case 0x301C: case 0xFF5E: return L"~";
    case 0x2026: return L"...";
    default: return nullptr;
    }
}

// Romanizes the kana of `w` (other characters copied; fullwidth ASCII made ASCII).
std::wstring kanaToLatin(std::wstring_view w) {
    std::string latin;   // ASCII built for the current kana run
    std::wstring out;
    bool sokuon = false;
    auto flushLatin = [&] {
        for (char ch : latin) out.push_back(static_cast<wchar_t>(ch));
        latin.clear();
    };
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (isKanaMark(c)) {   // ー: the vowel again
            if (!latin.empty() && isVowelChar(latin.back())) latin.push_back(latin.back());
            continue;
        }
        const wchar_t h = toHiragana(c);
        if (!isHiragana(h)) {
            sokuon = false;
            flushLatin();
            if (const wchar_t* p = punctuation(c)) {
                out += p;
            } else if (c >= 0xFF01 && c <= 0xFF5E) {
                out.push_back(static_cast<wchar_t>(c - 0xFEE0));
            } else {
                out.push_back(c);
            }
            continue;
        }
        if (h == 0x3063) {   // っ: doubles the next consonant
            sokuon = true;
            continue;
        }
        if (h == 0x3093) {   // ん: n' before a vowel or y
            latin += "n";
            if (i + 1 < w.size()) {
                const wchar_t nh = toHiragana(w[i + 1]);
                const std::string_view nx = isHiragana(nh) ? kKana[nh - 0x3041] : "";
                if (!nx.empty() && nh != 0x3093 && nh != 0x3063 && (isVowelChar(nx[0]) || nx[0] == 'y')) latin += "'";
            }
            continue;
        }
        std::string syl = kKana[h - 0x3041];
        const wchar_t next = i + 1 < w.size() ? toHiragana(w[i + 1]) : 0;
        const bool smallKana = isSmallY(h) || isSmallVowel(h);
        if (!smallKana && isSmallY(next) && syl.size() >= 2 && syl.back() == 'i') {   // きゃ kya, しゃ sha, じゃ ja
            const char v = next == 0x3083 ? 'a' : next == 0x3085 ? 'u' : 'o';
            syl.pop_back();
            if (syl != "sh" && syl != "ch" && syl != "j") syl += 'y';
            syl += v;
            ++i;
        } else if (!smallKana && isSmallVowel(next)) {   // ファ fa, ティ ti, ウィ wi, シェ she, イェ ye
            const char v = kKana[next - 0x3041][0];
            if (h == 0x3046) syl = std::string("w") + v;
            else if (h == 0x3044 && v == 'e') syl = "ye";
            else if (syl == "ku" || syl == "gu") syl = syl.substr(0, 1) + "w" + v;
            else if (syl.size() >= 2) syl = syl.substr(0, syl.size() - 1) + v;
            else syl += v;
            ++i;
        }
        if (sokuon && !syl.empty() && !isVowelChar(syl[0])) latin += syl.rfind("ch", 0) == 0 ? 't' : syl[0];
        sokuon = false;
        latin += syl;
    }
    flushLatin();
    // Spaces left doubled by the punctuation ("、 " then a space) and at the ends.
    std::wstring tidy;
    for (const wchar_t ch : out) {
        if (ch == L' ' && (tidy.empty() || tidy.back() == L' ')) continue;
        tidy.push_back(ch);
    }
    while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();
    return tidy;
}

bool hasHan(std::wstring_view w) { return std::any_of(w.begin(), w.end(), isHan); }

// Particles written apart when they end a phrase (は wa, へ e, を o as particles).
bool isParticle(std::wstring_view w) {
    static const wchar_t* const kParticles[] = {L"は", L"が", L"を", L"に", L"で", L"と", L"の", L"も", L"へ", L"や",
                                                L"か", L"ね", L"よ", L"から", L"まで", L"より", L"って"};
    return std::any_of(std::begin(kParticles), std::end(kParticles), [w](const wchar_t* p) { return w == p; });
}

std::wstring particleLatin(std::wstring_view w) {
    if (w == L"は") return L"wa";
    if (w == L"へ") return L"e";
    if (w == L"を") return L"o";
    return kanaToLatin(w);
}

// Kana runs romanized, kanji kept, a space where they meet (no readings to go by).
std::wstring kanaOnly(std::wstring_view w) {
    std::wstring out;
    size_t i = 0;
    while (i < w.size()) {
        const bool han = isHan(w[i]);
        size_t j = i;
        while (j < w.size() && isHan(w[j]) == han) ++j;
        std::wstring part = han ? std::wstring(w.substr(i, j - i)) : kanaToLatin(w.substr(i, j - i));
        if (!part.empty()) {
            if (!out.empty() && out.back() != L' ' && part.front() != L' ' && !std::iswpunct(part.front())) out += L' ';
            out += part;
        }
        i = j;
    }
    return out;
}

// ---- Cyrillic ------------------------------------------------------------------------------------------------------

wchar_t lowerCyr(wchar_t c) {
    wchar_t buf[2] = {c, 0};
    CharLowerBuffW(buf, 1);
    return buf[0];
}
bool upperCyr(wchar_t c) { return isCyrillic(c) && lowerCyr(c) != c; }
bool cyrVowel(wchar_t l) { return std::wstring_view(L"аеёиоуыэюяіїєу").find(l) != std::wstring_view::npos; }

// Lowercase Latin for one lowercase Cyrillic letter. `initial`: at the start of a word; `prev`: the previous letter
// (lowercase, 0 = none).
std::string cyrLatin(wchar_t l, bool uk, bool initial, wchar_t prev, wchar_t next) {
    if (uk) {
        switch (l) {
        case L'г': return prev == L'з' ? "gh" : "h";   // зг zgh
        case L'ґ': return "g";
        case L'е': return "e";
        case L'є': return initial ? "ye" : "ie";
        case L'и': return "y";
        case L'і': return "i";
        case L'ї': return initial ? "yi" : "i";
        case L'й': return initial ? "y" : "i";
        case L'ю': return initial ? "yu" : "iu";
        case L'я': return initial ? "ya" : "ia";
        case L'х': return "kh";
        case L'щ': return "shch";
        case L'ь': return "";
        default: break;
        }
    } else {
        switch (l) {
        case L'е': return initial || cyrVowel(prev) || prev == L'ъ' || prev == L'ь' ? "ye" : "e";
        case L'ё': return "yo";
        case L'и': return "i";
        case L'й': return "y";
        case L'ы': return "y";
        case L'э': return "e";
        case L'ю': return "yu";
        case L'я': return "ya";
        case L'ъ': case L'ь': return "";
        case L'х': return "kh";
        case L'щ': return "shch";
        case L'г': return "g";
        default: break;
        }
    }
    (void)next;
    switch (l) {
    case L'а': return "a";
    case L'б': return "b";
    case L'в': return "v";
    case L'г': return "g";
    case L'д': return "d";
    case L'е': return "e";
    case L'ж': return "zh";
    case L'з': return "z";
    case L'и': return "i";
    case L'к': return "k";
    case L'л': return "l";
    case L'м': return "m";
    case L'н': return "n";
    case L'о': return "o";
    case L'п': return "p";
    case L'р': return "r";
    case L'с': return "s";
    case L'т': return "t";
    case L'у': return "u";
    case L'ф': return "f";
    case L'ц': return "ts";
    case L'ч': return "ch";
    case L'ш': return "sh";
    case L'х': return "kh";
    case L'щ': return "shch";
    case L'й': return "y";
    case L'ю': return "yu";
    case L'я': return "ya";
    // Russian letters in a Ukrainian line (a quote, a name): the Russian reading rather than nothing.
    case L'ы': return "y";
    case L'э': return "e";
    case L'ё': return "yo";
    case L'ъ': case L'ь': return "";
    // Other Cyrillic alphabets, close enough.
    case L'і': return "i";
    case L'ї': return "yi";
    case L'є': return "ye";
    case L'ґ': return "g";
    case L'ў': return "w";
    case L'ђ': return "dj";
    case L'ј': return "j";
    case L'љ': return "lj";
    case L'њ': return "nj";
    case L'ћ': return "c";
    case L'џ': return "dz";
    case L'ѓ': return "gj";
    case L'ќ': return "kj";
    case L'ѕ': return "dz";
    default: return {};
    }
}

bool isApostrophe(wchar_t c) { return c == L'\'' || c == 0x2019 || c == 0x02BC; }

// The table for one line: its own letters decide (і ї є ґ Ukrainian, ы э ё ъ Russian), else the song's.
std::string lineCyrillicLanguage(std::wstring_view w, const std::string& song) {
    for (wchar_t c : w) {
        const wchar_t lc = lowerCyr(c);
        if (lc == L'і' || lc == L'ї' || lc == L'є' || lc == L'ґ') return "uk";
        if (lc == L'ы' || lc == L'э' || lc == L'ё' || lc == L'ъ') return "ru";
    }
    return song;
}

// ---- Cache ---------------------------------------------------------------------------------------------------------

std::string fingerprint(const Lyrics& l) {
    uint64_t h = 1469598103934665603ull;   // FNV-1a 64
    for (const auto& line : l.lines) {
        for (unsigned char c : line.text) h = (h ^ c) * 1099511628211ull;
        h = (h ^ '\n') * 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

fs::path cachePath(const std::string& key) {
    if (key.empty()) return {};
    std::string name;
    for (char c : key)
        name.push_back((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '_' ? c : '_');
    const auto dir = paths::cacheDir() / L"lyrics-romanized";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (name + ".json");
}

const char* scriptName(Script s) {
    switch (s) {
    case Script::Japanese: return "ja";
    case Script::Korean: return "ko";
    case Script::Cyrillic: return "cyrl";
    default: return "";
    }
}

Script scriptFrom(const std::string& s) {
    if (s == "ja") return Script::Japanese;
    if (s == "ko") return Script::Korean;
    if (s == "cyrl") return Script::Cyrillic;
    return Script::None;
}

void writeCache(const fs::path& p, const Lyrics& l, const Romanization& r) {
    if (p.empty()) return;
    const json j{{"v", kCacheVersion},          {"fp", fingerprint(l)},  {"n", l.lines.size()}, {"script", scriptName(r.script)},
                 {"lang", r.language},          {"readings", r.readings}, {"lines", r.lines}};
    auto tmp = p;
    tmp += L".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const std::string s = j.dump();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!f) return;
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) fs::remove(tmp, ec);
}

// ---- Windows' Japanese phonetic analyzer ---------------------------------------------------------------------------

std::atomic<int> g_analyzer{-1};   // -1 unknown, 0 gives nothing on this PC, 1 works

// Runs the analyzer on an STA thread of its own (JapanesePhoneticAnalyzer refuses the MTA of the worker threads), one
// request at a time, for the reader's lifetime.
class WinRtReader final : public JapaneseReader {
public:
    WinRtReader() : thread_([this] { run(); }) {}
    ~WinRtReader() override {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        thread_.join();
    }

    std::optional<std::vector<Word>> words(std::wstring_view text) override {
        if (g_analyzer.load() == 0) return std::nullopt;
        std::vector<Word> all;
        // The analyzer takes at most 100 characters at a time: longer text goes in pieces, cut after a space or
        // punctuation where there is one.
        size_t i = 0;
        while (i < text.size()) {
            size_t len = std::min<size_t>(100, text.size() - i);
            if (i + len < text.size()) {
                for (size_t k = len; k > 40; --k) {
                    const wchar_t c = text[i + k - 1];
                    if (isSpace(c) || c == 0x3001 || c == 0x3002 || c == L',' || c == L'!' || c == L'?') {
                        len = k;
                        break;
                    }
                }
            }
            auto part = ask(std::wstring(text.substr(i, len)));
            if (!part) return std::nullopt;
            all.insert(all.end(), part->begin(), part->end());
            i += len;
        }
        return all;
    }

private:
    std::optional<std::vector<Word>> ask(std::wstring text) {
        std::unique_lock lock(mutex_);
        job_ = std::move(text);
        hasJob_ = true;
        done_ = false;
        cv_.notify_all();
        cv_.wait(lock, [this] { return done_; });
        return std::move(result_);
    }

    void run() {
        const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::unique_lock lock(mutex_);
        for (;;) {
            cv_.wait(lock, [this] { return stop_ || hasJob_; });
            if (stop_) break;
            hasJob_ = false;
            const std::wstring text = std::move(job_);
            lock.unlock();
            std::optional<std::vector<Word>> out;
            try {
                const auto words = winrt::Windows::Globalization::JapanesePhoneticAnalyzer::GetWords(text, false);
                std::vector<Word> v;
                v.reserve(words.Size());
                for (const auto& w : words) v.push_back({std::wstring(w.DisplayText()), std::wstring(w.YomiText()), w.IsPhraseStart()});
                // Without the Japanese language data it answers nothing (kanji and kana alike).
                if (!v.empty()) {
                    g_analyzer = 1;
                    out = std::move(v);
                } else if (hasHan(text) || std::any_of(text.begin(), text.end(), isKana)) {
                    if (g_analyzer.exchange(0) != 0)
                        ST_LOG_INFO("lyrics", "Japanese phonetic analyzer gives no readings here: kana-only romanization");
                }
            } catch (const winrt::hresult_error& e) {
                g_analyzer = 0;
                ST_LOG_WARN("lyrics", "Japanese phonetic analyzer unavailable: 0x{:08x}", static_cast<uint32_t>(e.code()));
            } catch (...) {
                g_analyzer = 0;
            }
            lock.lock();
            result_ = std::move(out);
            done_ = true;
            cv_.notify_all();
        }
        lock.unlock();
        if (SUCCEEDED(co)) CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::wstring job_;
    std::optional<std::vector<Word>> result_;
    bool hasJob_ = false, done_ = false, stop_ = false;
    std::thread thread_;
};

} // namespace

// ---- Public ----------------------------------------------------------------------------------------------------------

std::unique_ptr<JapaneseReader> systemJapaneseReader() {
    if (g_analyzer.load() == 0) return nullptr;
    return std::make_unique<WinRtReader>();
}

std::string romanizeKorean(std::string_view utf8) {
    const std::wstring w = toWide(utf8);
    std::string out;
    size_t i = 0;
    while (i < w.size()) {
        if (!isHangul(w[i])) {
            const size_t j = std::find_if(w.begin() + static_cast<std::ptrdiff_t>(i), w.end(), isHangul) - w.begin();
            out += toUtf8(std::wstring_view(w).substr(i, j - i));
            i = j;
            continue;
        }
        std::vector<Syl> word;
        for (; i < w.size() && isHangul(w[i]); ++i) {
            const int s = w[i] - 0xAC00;
            word.push_back({s / 588, s % 588 / 28, s % 28});
        }
        out += romanizeHangulWord(word);
    }
    return out;
}

std::string romanizeKana(std::string_view utf8) { return toUtf8(kanaToLatin(toWide(utf8))); }

std::string romanizeJapanese(std::string_view utf8, JapaneseReader* reader, bool* readings) {
    const std::wstring w = toWide(utf8);
    const bool han = hasHan(w);
    if (readings) *readings = !han;
    std::optional<std::vector<JapaneseReader::Word>> words;
    if (reader && (han || std::any_of(w.begin(), w.end(), isKana))) words = reader->words(w);
    if (!words || words->empty()) return toUtf8(kanaOnly(w));
    std::wstring out;
    bool gotReadings = true;
    for (size_t k = 0; k < words->size(); ++k) {
        const auto& word = (*words)[k];
        if (std::all_of(word.text.begin(), word.text.end(), isSpace)) {
            if (!out.empty() && out.back() != L' ') out += L' ';
            continue;
        }
        const bool lastInPhrase = k + 1 == words->size() || (*words)[k + 1].phraseStart;
        const bool particle = isParticle(word.text) && lastInPhrase && k > 0;
        std::wstring latin;
        if (particle) {
            latin = particleLatin(word.text);
        } else if (!word.reading.empty() && std::all_of(word.reading.begin(), word.reading.end(), [](wchar_t c) {
                       return isKana(c) || isSpace(c);
                   })) {
            latin = kanaToLatin(word.reading);
        } else {
            if (hasHan(word.text)) gotReadings = false;
            latin = hasHan(word.text) ? kanaOnly(word.text) : kanaToLatin(word.text);
        }
        if (latin.empty()) continue;
        const bool punct = std::iswpunct(latin.front()) && latin.front() != L'"' && latin.front() != L'(';
        if (!out.empty() && out.back() != L' ' && !punct && (word.phraseStart || particle)) out += L' ';
        out += latin;
    }
    while (!out.empty() && out.back() == L' ') out.pop_back();
    if (readings) *readings = !han || gotReadings;
    return toUtf8(out);
}

std::string romanizeCyrillic(std::string_view utf8, std::string_view language) {
    const bool uk = language.rfind("uk", 0) == 0;
    const std::wstring w = toWide(utf8);
    std::string out;
    for (size_t i = 0; i < w.size(); ++i) {
        const wchar_t c = w[i];
        if (!isCyrillic(c)) {
            // An apostrophe inside a word (подвір'я) is not written.
            if (isApostrophe(c) && i > 0 && i + 1 < w.size() && isCyrillic(w[i - 1]) && isCyrillic(w[i + 1])) continue;
            out += toUtf8(std::wstring_view(&w[i], 1));
            continue;
        }
        const wchar_t l = lowerCyr(c);
        size_t p = i;   // the previous letter, past an in-word apostrophe
        if (p > 0 && isApostrophe(w[p - 1]) && p > 1 && isCyrillic(w[p - 2])) --p;
        const bool initial = p == 0 || !isCyrillic(w[p - 1]);
        const wchar_t prev = initial ? 0 : lowerCyr(w[p - 1]);
        const wchar_t next = i + 1 < w.size() && isCyrillic(w[i + 1]) ? lowerCyr(w[i + 1]) : 0;
        std::string latin = cyrLatin(l, uk, initial, prev, next);
        if (latin.empty()) continue;
        if (upperCyr(c)) {
            // All capitals (a shouted word) stay so; a capital before small letters starts a capitalized word.
            const bool neighbourUpper = (i + 1 < w.size() && upperCyr(w[i + 1])) || (!initial && upperCyr(w[i - 1]));
            if (neighbourUpper) {
                for (char& ch : latin) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            } else {
                latin[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(latin[0])));
            }
        }
        out += latin;
    }
    return out;
}

std::string cyrillicLanguage(const Lyrics& l) {
    // The lines with letters only one of the two alphabets has decide; a tie or none: the source's word, else Russian.
    int uk = 0, ru = 0;
    for (const auto& line : l.lines) {
        const std::string lang = lineCyrillicLanguage(toWide(line.text), "");
        uk += lang == "uk";
        ru += lang == "ru";
    }
    if (uk != ru) return uk > ru ? "uk" : "ru";
    return l.language.rfind("uk", 0) == 0 ? "uk" : "ru";
}

Script romanizableScript(const Lyrics& l) {
    size_t kana = 0, han = 0, hangul = 0, cyr = 0;
    for (const auto& line : l.lines)
        for (wchar_t c : toWide(line.text)) {
            if (isKana(c)) ++kana;
            else if (isHan(c)) ++han;
            else if (isHangul(c)) ++hangul;
            else if (isCyrillic(c)) ++cyr;
        }
    const size_t ja = kana > 0 ? kana + han : 0;   // kanji alone could be Chinese: not romanized
    const size_t best = std::max({ja, hangul, cyr});
    if (best < 3) return Script::None;
    if (best == ja) return Script::Japanese;
    if (best == hangul) return Script::Korean;
    return Script::Cyrillic;
}

std::optional<Romanization> cachedRomanization(const Lyrics& l, const std::string& cacheKey) {
    const fs::path p = cachePath(cacheKey);
    if (p.empty()) return std::nullopt;
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    const std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const json j = json::parse(data, nullptr, false);
    if (!j.is_object() || j.value("v", 0) != kCacheVersion || j.value("fp", std::string()) != fingerprint(l) ||
        j.value("n", size_t{0}) != l.lines.size())
        return std::nullopt;
    Romanization r;
    r.script = scriptFrom(j.value("script", std::string()));
    r.language = j.value("lang", std::string());
    r.readings = j.value("readings", true);
    if (auto it = j.find("lines"); it != j.end() && it->is_array())
        for (const auto& x : *it) r.lines.push_back(x.is_string() ? x.get<std::string>() : std::string());
    if (r.script == Script::None || r.lines.size() != l.lines.size()) return std::nullopt;
    return r;
}

std::optional<Romanization> romanize(const Lyrics& l, const std::string& cacheKey, JapaneseReader* reader) {
    Romanization r;
    r.script = romanizableScript(l);
    if (r.script == Script::None) return r;
    if (auto cached = cachedRomanization(l, cacheKey); cached && cached->script == r.script) return cached;
    r.language = r.script == Script::Japanese ? "ja" : r.script == Script::Korean ? "ko" : cyrillicLanguage(l);
    r.lines.reserve(l.lines.size());
    for (const auto& line : l.lines) {
        std::string latin;
        const std::wstring w = toWide(line.text);
        switch (r.script) {
        case Script::Japanese:
            if (std::any_of(w.begin(), w.end(), [](wchar_t c) { return isKana(c) || isHan(c); })) {
                bool got = true;
                latin = romanizeJapanese(line.text, reader, &got);
                r.readings = r.readings && got;
            }
            break;
        case Script::Korean:
            if (std::any_of(w.begin(), w.end(), isHangul)) latin = romanizeKorean(line.text);
            break;
        case Script::Cyrillic:
            if (std::any_of(w.begin(), w.end(), isCyrillic)) latin = romanizeCyrillic(line.text, lineCyrillicLanguage(w, r.language));
            break;
        default: break;
        }
        if (latin == line.text) latin.clear();
        r.lines.push_back(std::move(latin));
    }
    // A Japanese answer without kanji readings is made again next time (the language data may be installed by then).
    if (r.script != Script::Japanese || r.readings) writeCache(cachePath(cacheKey), l, r);
    return r;
}

} // namespace st::lyrics
