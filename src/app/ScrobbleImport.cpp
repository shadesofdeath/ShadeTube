// Last.fm / ListenBrainz history import: API paging, export files, scrobble records -> ListenStats rows.
#include "app/ScrobbleImport.h"

#include "app/HistoryImport.h"
#include "core/Http.h"
#include "core/Log.h"
#include "core/Utf.h"

#include <YoutubeExplode/Exceptions.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace st::app::scrobbles {

using json = nlohmann::json;
using YoutubeExplode::CancellationToken;
using YoutubeExplode::Http::Headers;
using YoutubeExplode::Http::HttpResponse;

namespace {

constexpr const char* kTag = "stats";
constexpr const char* kLastfmApi = "https://ws.audioscrobbler.com/2.0/";
constexpr const char* kLbApi = "https://api.listenbrainz.org/1/";
constexpr const char* kUserAgent = "ShadeTube (Windows; history import)";
constexpr size_t kMaxHeldNodes = 200'000;   // JSON nodes kept outside records before a file counts as no history
constexpr const char* kLastfmNoImage = "2a96cbd8b46e442fc41c2b86b821562f";   // Last.fm's grey star placeholder

int64_t unixNow() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

std::string lowerAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; });
    return out;
}

std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return std::string(s.substr(a, b - a));
}

std::string_view stripBom(std::string_view s) { return s.rfind("\xEF\xBB\xBF", 0) == 0 ? s.substr(3) : s; }

bool isMbid(std::string_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23 ? c != '-' : !std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}
std::string mbidOrEmpty(std::string_view id) { return isMbid(id) ? lowerAscii(id) : std::string{}; }

const json* member(const json& o, const char* key) {
    if (!o.is_object()) return nullptr;
    const auto it = o.find(key);
    return it == o.end() || it->is_null() ? nullptr : &*it;
}

std::string str(const json& o, const char* key) {
    const json* v = member(o, key);
    return v && v->is_string() ? v->get<std::string>() : std::string{};
}

// Last.fm's text members: a string, or an object {"#text": ...} (or {"name": ...} with extended=1).
std::string lfmText(const json& o, const char* key) {
    const json* v = member(o, key);
    if (!v) return {};
    if (v->is_string()) return v->get<std::string>();
    if (v->is_object()) {
        std::string t = str(*v, "#text");
        return t.empty() ? str(*v, "name") : t;
    }
    return {};
}

int64_t number(const json& v) {
    if (v.is_number_integer()) return v.get<int64_t>();
    if (v.is_number_unsigned()) return static_cast<int64_t>(std::min<uint64_t>(v.get<uint64_t>(), static_cast<uint64_t>(std::numeric_limits<int64_t>::max())));
    if (v.is_number_float()) return static_cast<int64_t>(std::llround(v.get<double>()));
    return -1;
}

// A date value: a number (unix seconds, or ms when that large), a string (parseDate), or Last.fm's {"uts", "#text"}.
int64_t dateOf(const json& v) {
    if (v.is_number()) {
        const int64_t n = number(v);
        return n > 100'000'000'000LL ? n / 1000 : n;
    }
    if (v.is_string()) return parseDate(v.get_ref<const std::string&>());
    if (v.is_object()) {
        if (const json* u = member(v, "uts")) return dateOf(*u);
        if (const json* t = member(v, "#text")) return dateOf(*t);
    }
    return -1;
}

int digitsAt(std::string_view s, size_t& at, int maxDigits) {
    int v = 0, n = 0;
    while (at < s.size() && n < maxDigits && s[at] >= '0' && s[at] <= '9') {
        v = v * 10 + (s[at] - '0');
        ++at;
        ++n;
    }
    return n ? v : -1;
}

int monthOf(std::string_view m) {
    static const char* const kMonths[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    if (m.size() < 3) return 0;
    const std::string l = lowerAscii(m.substr(0, 3));
    for (int i = 0; i < 12; ++i)
        if (l == kMonths[i]) return i + 1;
    return 0;
}

// "14 Nov 2023, 22:13" / "14 Nov 2023 22:13" / "1 November 2023, 09:05:30" (Last.fm's UTC text).
int64_t parseLastfmText(std::string_view s) {
    size_t at = 0;
    const int day = digitsAt(s, at, 2);
    if (day < 1 || day > 31 || at >= s.size() || s[at] != ' ') return -1;
    ++at;
    const size_t monthStart = at;
    while (at < s.size() && std::isalpha(static_cast<unsigned char>(s[at]))) ++at;
    const int month = monthOf(s.substr(monthStart, at - monthStart));
    if (!month || at >= s.size() || s[at] != ' ') return -1;
    ++at;
    const int year = digitsAt(s, at, 4);
    if (year < 1970) return -1;
    if (at < s.size() && s[at] == ',') ++at;
    while (at < s.size() && s[at] == ' ') ++at;
    int h = 0, mi = 0, sec = 0;
    if (at < s.size()) {
        h = digitsAt(s, at, 2);
        if (h < 0 || h > 23 || at >= s.size() || s[at] != ':') return -1;
        ++at;
        mi = digitsAt(s, at, 2);
        if (mi < 0 || mi > 59) return -1;
        if (at < s.size() && s[at] == ':') {
            ++at;
            sec = digitsAt(s, at, 2);
            if (sec < 0 || sec > 60) return -1;
        }
        if (at != s.size()) return -1;
    }
    return listen::daysFromCivil(year, month, day) * 86400 + h * 3600 + mi * 60 + sec;
}

// Fills a scrobble row from the pieces every Last.fm format has.
bool makeRow(std::string title, std::string artist, std::string artistMbid, std::string album, std::string trackMbid,
             std::string image, int64_t startedAt, Row& out) {
    title = trim(title);
    artist = trim(artist);
    if (title.empty() || artist.empty() || startedAt < kUndatedBefore) return false;
    out = {};
    out.track.name = std::move(title);
    out.track.artists.push_back({mbidOrEmpty(artistMbid), std::move(artist)});
    out.track.albumName = trim(album);
    out.track.id = mbidOrEmpty(trackMbid);
    if (out.track.id.empty()) out.track.id = history::importId(out.track.name, out.track.artists[0].name);
    out.track.image = std::move(image);
    out.startedAt = startedAt;
    return true;
}

// Last.fm's image list: the 300 px one ("extralarge"), else the largest, never the placeholder star.
std::string lfmImage(const json& r) {
    const json* images = member(r, "image");
    if (!images || !images->is_array()) return {};
    std::string best;
    int bestRank = -1;
    for (const auto& im : *images) {
        const std::string url = str(im, "#text");
        if (url.empty() || url.find(kLastfmNoImage) != std::string::npos) continue;
        const std::string size = str(im, "size");
        const int rank = size == "extralarge" ? 4 : size == "large" ? 3 : size == "mega" ? 2 : size == "medium" ? 1 : 0;
        if (rank > bestRank) {
            bestRank = rank;
            best = url;
        }
    }
    return best;
}

} // namespace

ImportSource sourceOf(Service s) { return s == Service::LastFm ? ImportSource::LastFm : ImportSource::ListenBrainz; }

int64_t parseDate(std::string_view raw) {
    const std::string t = trim(stripBom(raw));
    if (t.empty()) return -1;
    if (std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        if (t.size() > 15) return -1;
        int64_t n = 0;
        std::from_chars(t.data(), t.data() + t.size(), n);
        return n > 100'000'000'000LL ? n / 1000 : n;   // milliseconds (lastfmstats) or seconds
    }
    if (const int64_t iso = history::parseUtc(t); iso >= 0) return iso;
    return parseLastfmText(t);
}

Shape shapeOf(const json& r) {
    if (!r.is_object()) return Shape::None;
    if (const json* tm = member(r, "track_metadata"); tm && tm->is_object() && member(r, "listened_at")) return Shape::ListenBrainz;
    const bool title = member(r, "name") || member(r, "track") || member(r, "title") || member(r, "track_name");
    const bool artist = member(r, "artist") || member(r, "artist_name");
    const bool date = member(r, "date") || member(r, "uts") || member(r, "timestamp") || member(r, "played_at");
    return title && artist && date ? Shape::LastFm : Shape::None;
}

bool lastfmRow(const json& r, Row& out) {
    if (!r.is_object()) return false;
    if (const json* attr = member(r, "@attr")) {
        if (const json* np = member(*attr, "nowplaying"); np && ((np->is_string() && np->get<std::string>() == "true") || (np->is_boolean() && np->get<bool>())))
            return false;
    }
    std::string title = lfmText(r, "name");
    if (title.empty()) title = lfmText(r, "track");
    if (title.empty()) title = str(r, "title");
    if (title.empty()) title = str(r, "track_name");
    std::string artist = lfmText(r, "artist"), artistMbid;
    if (const json* a = member(r, "artist"); a && a->is_object()) artistMbid = str(*a, "mbid");
    if (artist.empty()) artist = str(r, "artist_name");
    if (artistMbid.empty()) artistMbid = str(r, "artist_mbid");
    std::string album = lfmText(r, "album");
    if (album.empty()) album = str(r, "album_name");
    std::string trackMbid = str(r, "mbid");
    if (trackMbid.empty()) trackMbid = str(r, "track_mbid");
    int64_t at = -1;
    for (const char* key : {"date", "uts", "timestamp", "played_at"})
        if (const json* d = member(r, key)) {
            at = dateOf(*d);
            if (at >= 0) break;
        }
    return makeRow(std::move(title), std::move(artist), std::move(artistMbid), std::move(album), std::move(trackMbid),
                   lfmImage(r), at, out);
}

bool listenbrainzRow(const json& l, Row& out) {
    const json* tm = member(l, "track_metadata");
    const json* when = member(l, "listened_at");
    if (!tm || !tm->is_object() || !when) return false;
    const int64_t at = dateOf(*when);
    std::string title = trim(str(*tm, "track_name")), credit = trim(str(*tm, "artist_name"));
    if (title.empty() || credit.empty() || at < kUndatedBefore) return false;
    static const json kEmpty = json::object();
    const json* add = member(*tm, "additional_info");
    const json* map = member(*tm, "mbid_mapping");
    if (!add || !add->is_object()) add = &kEmpty;
    if (!map || !map->is_object()) map = &kEmpty;
    out = {};
    out.track.name = std::move(title);
    out.track.albumName = trim(str(*tm, "release_name"));
    // Artists: MusicBrainz's credit when the listen is mapped, else what the client sent (names + MBIDs), else the text.
    if (const json* as = member(*map, "artists"); as && as->is_array())
        for (const auto& a : *as) {
            std::string name = trim(str(a, "artist_credit_name"));
            if (!name.empty()) out.track.artists.push_back({mbidOrEmpty(str(a, "artist_mbid")), std::move(name)});
        }
    if (out.track.artists.empty())
        if (const json* names = member(*add, "artist_names"); names && names->is_array()) {
            const json* ids = member(*add, "artist_mbids");
            for (size_t i = 0; i < names->size(); ++i) {
                if (!(*names)[i].is_string()) continue;
                std::string name = trim((*names)[i].get<std::string>());
                std::string id = ids && ids->is_array() && ids->size() == names->size() && (*ids)[i].is_string()
                                     ? mbidOrEmpty((*ids)[i].get<std::string>())
                                     : std::string{};
                if (!name.empty()) out.track.artists.push_back({std::move(id), std::move(name)});
            }
        }
    if (out.track.artists.empty()) {
        std::string id;
        if (const json* ids = member(*add, "artist_mbids"); ids && ids->is_array() && ids->size() == 1 && (*ids)[0].is_string())
            id = mbidOrEmpty((*ids)[0].get<std::string>());
        out.track.artists.push_back({std::move(id), credit});
    }
    std::string recording = mbidOrEmpty(str(*map, "recording_mbid"));
    if (recording.empty()) recording = mbidOrEmpty(str(*add, "recording_mbid"));
    out.track.id = !recording.empty() ? recording : history::importId(out.track.name, out.track.artists[0].name);
    std::string group = mbidOrEmpty(str(*map, "release_group_mbid"));
    if (group.empty()) group = mbidOrEmpty(str(*add, "release_group_mbid"));
    if (!out.track.albumName.empty()) out.track.albumId = std::move(group);
    if (const json* d = member(*add, "duration_ms"); d && d->is_number()) out.track.durationMs = std::max<int64_t>(0, number(*d));
    else if (const json* s = member(*add, "duration"); s && s->is_number()) out.track.durationMs = std::max<int64_t>(0, number(*s)) * 1000;
    if (out.track.durationMs > 24 * 3600 * 1000LL) out.track.durationMs = 0;   // nonsense
    if (const std::string release = mbidOrEmpty(str(*map, "caa_release_mbid")); !release.empty()) {
        const json* caa = member(*map, "caa_id");
        const int64_t id = caa ? number(*caa) : -1;
        out.track.image = "https://coverartarchive.org/release/" + release + "/" +
                          (id > 0 ? std::to_string(id) + "-250.jpg" : std::string("front-250"));
    }
    out.startedAt = at;
    return true;
}

LastfmPage parseLastfmPage(std::string_view body) {
    LastfmPage page;
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) {
        page.error = -1;
        return page;
    }
    if (const json* e = member(j, "error")) {
        page.error = e->is_number() ? static_cast<int>(number(*e)) : -1;
        page.message = str(j, "message");
        return page;
    }
    const json* rt = member(j, "recenttracks");
    if (!rt || !rt->is_object()) {
        page.error = -1;
        return page;
    }
    if (const json* attr = member(*rt, "@attr")) {   // numbers as strings ("12"), numbers in some proxies' copies
        page.page = std::atoi(str(*attr, "page").c_str());
        page.totalPages = std::atoi(str(*attr, "totalPages").c_str());
        page.total = std::atoll(str(*attr, "total").c_str());
        if (const json* p = member(*attr, "page"); p && p->is_number()) page.page = static_cast<int>(number(*p));
        if (const json* p = member(*attr, "totalPages"); p && p->is_number()) page.totalPages = static_cast<int>(number(*p));
        if (const json* p = member(*attr, "total"); p && p->is_number()) page.total = number(*p);
    }
    if (const json* tracks = member(*rt, "track")) {
        auto take = [&page](const json& t) {
            Row row;
            if (lastfmRow(t, row)) page.rows.push_back(std::move(row));
        };
        if (tracks->is_array())
            for (const auto& t : *tracks) take(t);
        else if (tracks->is_object())   // a single scrobble comes as an object
            take(*tracks);
    }
    return page;
}

LbPage parseLbPage(std::string_view body) {
    LbPage page;
    const json j = json::parse(body, nullptr, false);
    if (!j.is_object()) return page;
    page.error = str(j, "error");
    const json* p = member(j, "payload");
    if (!p || !p->is_object()) return page;
    const json* listens = member(*p, "listens");
    if (!listens || !listens->is_array()) return page;
    page.ok = true;
    if (const json* v = member(*p, "latest_listen_ts")) page.latest = number(*v);
    if (const json* v = member(*p, "oldest_listen_ts")) page.oldest = number(*v);
    page.listens = listens->size();
    for (const auto& l : *listens) {
        if (const json* w = member(l, "listened_at")) page.newestOnPage = std::max(page.newestOnPage, dateOf(*w));
        Row row;
        if (listenbrainzRow(l, row)) page.rows.push_back(std::move(row));
    }
    return page;
}

// ============================================================================================================
// Files

namespace {

// What a reader handed out.
struct ReadCount {
    size_t rows = 0, skipped = 0, foreign = 0;   // foreign: records of the other service
};

bool cancelled(const std::atomic<bool>* cancel) { return cancel && cancel->load(); }

// One record of a file, whatever its shape: converted when it is the service's, else counted.
void takeRecord(const json& rec, Shape shape, Service service, const Sink& sink, ReadCount& n) {
    const bool mine = (shape == Shape::LastFm) == (service == Service::LastFm);
    if (!mine) {
        ++n.foreign;
        return;
    }
    Row row;
    if (shape == Shape::LastFm ? lastfmRow(rec, row) : listenbrainzRow(rec, row)) {
        sink(std::move(row));
        ++n.rows;
    } else {
        ++n.skipped;
    }
}

// SAX reader: builds each object, and hands it out as soon as it closes when it is a scrobble record - it is then not
// attached to its parent, so the arrays around records stay empty and memory holds one record at a time.
class RecordSax final : public json::json_sax_t {
public:
    RecordSax(Service service, const Sink& sink, ReadCount& n, const std::atomic<bool>* cancel)
        : service_(service), sink_(sink), n_(n), cancel_(cancel) {}

    bool null() override { return put(json(nullptr)); }
    bool boolean(bool v) override { return put(json(v)); }
    bool number_integer(number_integer_t v) override { return put(json(v)); }
    bool number_unsigned(number_unsigned_t v) override { return put(json(v)); }
    bool number_float(number_float_t v, const string_t&) override { return put(json(v)); }
    bool string(string_t& v) override { return put(json(std::move(v))); }
    bool binary(binary_t&) override { return put(json(nullptr)); }
    bool start_object(std::size_t) override {
        stack_.emplace_back(json::object());
        keys_.emplace_back();
        return true;
    }
    bool key(string_t& k) override {
        keys_.back() = std::move(k);
        return true;
    }
    bool end_object() override {
        json v = std::move(stack_.back());
        stack_.pop_back();
        keys_.pop_back();
        if (const Shape s = shapeOf(v); s != Shape::None) {
            takeRecord(v, s, service_, sink_, n_);
            held_ = 0;
            if ((++records_ & 255) == 0 && cancelled(cancel_)) {
                stopped_ = true;
                return false;
            }
            return true;
        }
        return put(std::move(v));
    }
    bool start_array(std::size_t) override {
        stack_.emplace_back(json::array());
        keys_.emplace_back();
        return true;
    }
    bool end_array() override {
        json v = std::move(stack_.back());
        stack_.pop_back();
        keys_.pop_back();
        return put(std::move(v));
    }
    bool parse_error(std::size_t position, const std::string&, const nlohmann::detail::exception&) override {
        error_ = "not valid JSON (at byte " + std::to_string(position) + ")";
        return false;
    }
    const std::string& error() const { return error_; }
    bool stopped() const { return stopped_; }

private:
    bool put(json v) {
        if (stack_.empty()) return true;   // the document itself closed (or a bare scalar): nothing to keep
        if (++held_ > kMaxHeldNodes) {
            error_ = "no scrobbles found";
            return false;
        }
        json& top = stack_.back();
        if (top.is_array()) top.push_back(std::move(v));
        else top[keys_.back()] = std::move(v);
        return true;
    }

    Service service_;
    const Sink& sink_;
    ReadCount& n_;
    const std::atomic<bool>* cancel_;
    std::vector<json> stack_;
    std::vector<std::string> keys_;
    size_t held_ = 0, records_ = 0;
    bool stopped_ = false;
    std::string error_;
};

ReadCount readJsonStream(std::istream& in, Service service, const Sink& sink, const std::atomic<bool>* cancel) {
    ReadCount n;
    RecordSax sax(service, sink, n, cancel);
    const bool ok = json::sax_parse(in, &sax);
    if (!ok && !sax.stopped() && n.rows == 0) throw std::runtime_error(sax.error().empty() ? "not valid JSON" : sax.error());
    if (!ok && !sax.stopped()) ST_LOG_WARN(kTag, "import: {} after {} scrobbles", sax.error(), n.rows);
    return n;
}

// JSON lines: one record per line (blank lines skipped). The first line that is not JSON makes it no JSON-lines file.
void takeJsonLine(std::string_view line, Service service, const Sink& sink, ReadCount& n, size_t& lines, size_t& bad) {
    line = stripBom(line);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
    if (line.empty()) return;
    ++lines;
    const json rec = json::parse(line, nullptr, false);
    if (rec.is_discarded()) {
        if (lines == 1) throw std::runtime_error("not a JSON lines file");
        ++bad;
        return;
    }
    if (const Shape s = shapeOf(rec); s != Shape::None) takeRecord(rec, s, service, sink, n);
}

ReadCount readLines(std::string_view text, Service service, const Sink& sink, const std::atomic<bool>* cancel) {
    ReadCount n;
    size_t lines = 0, bad = 0;
    for (size_t at = 0; at < text.size();) {
        const size_t end = std::min(text.find('\n', at), text.size());
        takeJsonLine(text.substr(at, end - at), service, sink, n, lines, bad);
        at = end + 1;
        if ((lines & 255) == 0 && cancelled(cancel)) break;
    }
    if (bad) ST_LOG_WARN(kTag, "import: {} unreadable lines", bad);
    return n;
}

ReadCount readLinesStream(std::istream& in, Service service, const Sink& sink, const std::atomic<bool>* cancel) {
    ReadCount n;
    size_t lines = 0, bad = 0;
    std::string line;
    while (std::getline(in, line)) {
        takeJsonLine(line, service, sink, n, lines, bad);
        if ((lines & 255) == 0 && cancelled(cancel)) break;
    }
    if (bad) ST_LOG_WARN(kTag, "import: {} unreadable lines", bad);
    return n;
}

// ---- CSV ----------------------------------------------------------------------------------------------------

// One record (RFC 4180: quoted cells may hold delimiters, quotes as "" and line breaks). False at the end.
bool csvRecord(std::streambuf* sb, char delim, std::vector<std::string>& cells) {
    cells.clear();
    std::string cur;
    bool quoted = false, any = false;
    for (;;) {
        const int c = sb->sbumpc();
        if (c == std::char_traits<char>::eof()) {
            if (!any) return false;
            cells.push_back(std::move(cur));
            return true;
        }
        any = true;
        const char ch = static_cast<char>(c);
        if (quoted) {
            if (ch != '"') {
                cur += ch;
            } else if (sb->sgetc() == '"') {
                sb->sbumpc();
                cur += '"';
            } else {
                quoted = false;
            }
        } else if (ch == '"' && cur.empty()) {
            quoted = true;
        } else if (ch == delim) {
            cells.push_back(std::move(cur));
            cur.clear();
        } else if (ch == '\n' || ch == '\r') {
            if (ch == '\r' && sb->sgetc() == '\n') sb->sbumpc();
            cells.push_back(std::move(cur));
            return true;
        } else {
            cur += ch;
        }
    }
}

std::string headerName(std::string_view cell) {
    std::string h = lowerAscii(trim(stripBom(cell)));
    if (const size_t hash = h.find('#'); hash != std::string::npos) h.resize(hash);   // lastfmstats: "Date#<user>"
    for (char& c : h)
        if (c == ' ' || c == '-') c = '_';
    return h;
}

struct CsvColumns {
    int artist = -1, album = -1, track = -1, uts = -1, date = -1, artistMbid = -1, trackMbid = -1;
};

CsvColumns headerColumns(const std::vector<std::string>& cells) {
    CsvColumns c;
    auto is = [](const std::string& h, std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(), [&h](const char* n) { return h == n; });
    };
    for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
        const std::string h = headerName(cells[static_cast<size_t>(i)]);
        if (c.artist < 0 && is(h, {"artist", "artist_name", "artistname", "artists"})) c.artist = i;
        else if (c.album < 0 && is(h, {"album", "album_name", "albumname", "release", "release_name"})) c.album = i;
        else if (c.track < 0 && is(h, {"track", "title", "name", "track_name", "trackname", "song", "song_name"})) c.track = i;
        else if (c.uts < 0 && is(h, {"uts", "timestamp", "unix", "unix_timestamp", "listened_at", "time_stamp"})) c.uts = i;
        else if (c.date < 0 && is(h, {"date", "utc_time", "time", "played_at", "datetime", "date_time", "scrobble_time"})) c.date = i;
        else if (c.artistMbid < 0 && is(h, {"artist_mbid", "artistmbid", "artist_id"})) c.artistMbid = i;
        else if (c.trackMbid < 0 && is(h, {"track_mbid", "trackmbid", "mbid", "recording_mbid"})) c.trackMbid = i;
    }
    return c;
}

const std::string& cell(const std::vector<std::string>& cells, int i) {
    static const std::string none;
    return i >= 0 && static_cast<size_t>(i) < cells.size() ? cells[static_cast<size_t>(i)] : none;
}

ReadCount readCsvStream(std::istream& in, const Sink& sink, const std::atomic<bool>* cancel) {
    // The delimiter: whichever of , ; tab is most frequent outside quotes on the first line.
    const auto start = in.tellg();
    std::string first;
    std::getline(in, first);
    in.clear();
    in.seekg(start);
    size_t commas = 0, semicolons = 0, tabs = 0;
    bool q = false;
    for (const char c : first) {
        if (c == '"') q = !q;
        else if (!q && c == ',') ++commas;
        else if (!q && c == ';') ++semicolons;
        else if (!q && c == '\t') ++tabs;
    }
    const char delim = tabs > commas && tabs >= semicolons ? '\t' : semicolons > commas ? ';' : ',';

    ReadCount n;
    std::streambuf* sb = in.rdbuf();
    std::vector<std::string> cells;
    if (!csvRecord(sb, delim, cells)) throw std::runtime_error("empty file");
    CsvColumns col = headerColumns(cells);
    const bool header = col.artist >= 0 && col.track >= 0 && (col.uts >= 0 || col.date >= 0);
    bool pending = !header;   // headerless: the first record is a scrobble already
    if (!header) {
        // lastfm-to-csv (benjaminbenben.com): artist, album, track, date - no header.
        if (cells.size() < 4 || parseDate(cells[3]) < 0) throw std::runtime_error("not a scrobble CSV (unknown columns)");
        col = {};
        col.artist = 0;
        col.album = 1;
        col.track = 2;
        col.date = 3;
        if (!cells.empty()) cells[0] = std::string(stripBom(cells[0]));
    }
    size_t records = 0;
    while (pending || csvRecord(sb, delim, cells)) {
        pending = false;
        if (std::all_of(cells.begin(), cells.end(), [](const std::string& c) { return c.empty(); })) continue;
        int64_t at = col.uts >= 0 ? parseDate(cell(cells, col.uts)) : -1;
        if (at < 0 && col.date >= 0) at = parseDate(cell(cells, col.date));
        Row row;
        if (makeRow(cell(cells, col.track), cell(cells, col.artist), cell(cells, col.artistMbid), cell(cells, col.album),
                    cell(cells, col.trackMbid), {}, at, row)) {
            sink(std::move(row));
            ++n.rows;
        } else {
            ++n.skipped;
        }
        if ((++records & 255) == 0 && cancelled(cancel)) break;
    }
    return n;
}

enum class Kind { Csv, Json, JsonLines, Zip, Unknown };

Kind kindOf(const std::filesystem::path& p) {
    const std::string ext = lowerAscii(toUtf8(p.extension().wstring()));
    if (ext == ".csv" || ext == ".tsv" || ext == ".txt") return Kind::Csv;
    if (ext == ".json") return Kind::Json;
    if (ext == ".jsonl" || ext == ".ndjson") return Kind::JsonLines;
    if (ext == ".zip") return Kind::Zip;
    return Kind::Unknown;
}

Kind kindOfName(std::string_view name) {
    const std::string l = lowerAscii(name);
    auto ends = [&l](std::string_view e) { return l.size() >= e.size() && l.compare(l.size() - e.size(), e.size(), e) == 0; };
    if (ends(".jsonl") || ends(".ndjson")) return Kind::JsonLines;
    if (ends(".json")) return Kind::Json;
    if (ends(".csv") || ends(".tsv")) return Kind::Csv;
    return Kind::Unknown;
}

// A ListenBrainz export's other files (user info, feedback, pins): not listens.
bool notListens(std::string_view entry) {
    const size_t slash = entry.find_last_of('/');
    const std::string base = lowerAscii(slash == std::string_view::npos ? entry : entry.substr(slash + 1));
    return base == "user.jsonl" || base == "feedback.jsonl" || base == "pinned_recording.jsonl" || base.rfind("feedback", 0) == 0;
}

} // namespace

size_t readCsv(std::istream& in, const Sink& sink, size_t& skipped, const std::atomic<bool>* cancel) {
    const ReadCount n = readCsvStream(in, sink, cancel);
    skipped += n.skipped;
    return n.rows;
}

size_t readJson(std::istream& in, Service service, const Sink& sink, size_t& skipped, const std::atomic<bool>* cancel) {
    const ReadCount n = readJsonStream(in, service, sink, cancel);
    skipped += n.skipped;
    return n.rows;
}

size_t readJsonLines(std::istream& in, Service service, const Sink& sink, size_t& skipped, const std::atomic<bool>* cancel) {
    const ReadCount n = readLinesStream(in, service, sink, cancel);
    skipped += n.skipped;
    return n.rows;
}

FileStats readFiles(Service service, const std::vector<std::filesystem::path>& files, const Sink& sink,
                    const FileProgress& progress, const std::atomic<bool>* cancel) {
    FileStats st;
    struct Doc {
        size_t file;
        Kind kind;
        std::optional<history::ZipEntry> entry;
    };
    std::vector<Doc> docs;
    auto label = [&files](const Doc& d) {
        const std::string name = toUtf8(files[d.file].filename().wstring());
        return d.entry ? name + "/" + d.entry->name : name;
    };
    for (size_t i = 0; i < files.size(); ++i) {
        const Kind kind = kindOf(files[i]);
        try {
            if (kind == Kind::Unknown) throw std::runtime_error("not a .csv, .json, .jsonl or .zip file");
            if (kind != Kind::Zip) {
                docs.push_back({i, kind, std::nullopt});
                continue;
            }
            int found = 0;
            for (auto& e : history::zipEntries(files[i])) {
                const Kind k = kindOfName(e.name);
                if (k == Kind::Unknown || e.name.empty() || e.name.back() == '/' || notListens(e.name)) continue;
                docs.push_back({i, k, std::move(e)});
                ++found;
            }
            if (!found) ++st.unrecognized;
        } catch (const std::exception& e) {
            st.errors.push_back(toUtf8(files[i].filename().wstring()) + ": " + e.what());
        }
    }
    // Rows are counted on their way to the sink, so a long single file shows progress too.
    size_t rows = 0;
    int done = 0;
    const int total = static_cast<int>(docs.size());
    const Sink counted = [&](Row&& r) {
        sink(std::move(r));
        if ((++rows % 5000) == 0 && progress) progress(done, total, rows);
    };
    if (progress) progress(0, total, 0);
    for (const auto& d : docs) {
        if (cancelled(cancel)) {
            st.cancelled = true;
            break;
        }
        try {
            ReadCount n;
            if (d.entry) {
                const std::string content = history::zipRead(files[d.file], *d.entry);
                if (d.kind == Kind::JsonLines) {
                    n = readLines(content, service, counted, cancel);
                } else {
                    std::istringstream in(content);
                    n = d.kind == Kind::Json ? readJsonStream(in, service, counted, cancel) : readCsvStream(in, counted, cancel);
                }
            } else {
                std::ifstream in(files[d.file], std::ios::binary);
                if (!in) throw std::runtime_error("cannot open the file");
                if (d.kind == Kind::Csv && service != Service::LastFm) throw std::runtime_error("ListenBrainz exports are .zip / .jsonl / .json");
                n = d.kind == Kind::Json        ? readJsonStream(in, service, counted, cancel)
                    : d.kind == Kind::JsonLines ? readLinesStream(in, service, counted, cancel)
                                                : readCsvStream(in, counted, cancel);
            }
            st.skipped += n.skipped;
            if (n.rows > 0) ++st.documents;
            else ++st.unrecognized;
            ST_LOG_INFO(kTag, "import ({}): {} -> {} scrobbles, {} skipped{}", importSourceName(sourceOf(service)), label(d),
                        n.rows, n.skipped, n.foreign ? ", " + std::to_string(n.foreign) + " of the other service" : std::string());
        } catch (const std::exception& e) {
            st.errors.push_back(label(d) + ": " + e.what());
        }
        if (progress) progress(++done, total, rows);
    }
    st.rows = rows;
    if (cancelled(cancel)) st.cancelled = true;
    for (const auto& e : st.errors) ST_LOG_WARN(kTag, "import: {}", e);
    return st;
}

// ============================================================================================================
// APIs

int retryAfterSeconds(const HttpResponse& r, int64_t nowUnix) {
    const auto v = r.header("Retry-After");
    if (!v) return -1;
    const std::string t = trim(*v);
    if (!t.empty() && std::all_of(t.begin(), t.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return t.size() > 6 ? -1 : std::atoi(t.c_str());
    // HTTP date: "Wed, 21 Oct 2015 07:28:00 GMT"
    const size_t comma = t.find(", ");
    if (comma == std::string::npos) return -1;
    std::string rest = t.substr(comma + 2);
    if (rest.size() > 4 && rest.compare(rest.size() - 4, 4, " GMT") == 0) rest.resize(rest.size() - 4);
    const int64_t at = parseLastfmText(rest);
    if (at < 0) return -1;
    return static_cast<int>(std::max<int64_t>(0, at - nowUnix));
}

Transport liveTransport() {
    Transport t;
    t.get = [](const std::string& url, const Headers& headers, const CancellationToken& ct) { return http::get(url, headers, ct); };
    t.sleep = [](std::chrono::milliseconds d, const CancellationToken& ct) {
        const auto end = std::chrono::steady_clock::now() + d;
        for (;;) {
            ct.throwIfCancellationRequested();
            const auto now = std::chrono::steady_clock::now();
            if (now >= end) return;
            std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(end - now, std::chrono::milliseconds(50)));
        }
    };
    return t;
}

namespace {

int headerInt(const HttpResponse& r, const char* name, int fallback) {
    const auto v = r.header(name);
    if (!v) return fallback;
    int n = fallback;
    const std::string t = trim(*v);
    std::from_chars(t.data(), t.data() + t.size(), n);
    return n;
}

std::chrono::milliseconds backoff(int attempt) { return std::chrono::milliseconds(std::min<int64_t>(60'000, 2'000LL << std::min(attempt, 5))); }

// One request at a time, kMinGapMs apart; waits are reported (rate limits) and cancellable.
class Polite {
public:
    Polite(const Transport& t, const CancellationToken& ct, const std::function<void(int)>& waiting)
        : t_(t), ct_(ct), waiting_(waiting) {}
    HttpResponse get(const std::string& url, const Headers& headers) {
        const auto now = std::chrono::steady_clock::now();
        if (now < next_) t_.sleep(std::chrono::duration_cast<std::chrono::milliseconds>(next_ - now), ct_);
        ct_.throwIfCancellationRequested();
        ++requests;
        HttpResponse r = t_.get(url, headers, ct_);
        next_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMinGapMs);
        return r;
    }
    void wait(std::chrono::milliseconds d, bool rateLimit) {
        if (rateLimit && waiting_) waiting_(static_cast<int>((d.count() + 999) / 1000));
        t_.sleep(d, ct_);
        if (rateLimit && waiting_) waiting_(0);
    }
    void notBefore(std::chrono::milliseconds d) { next_ = std::max(next_, std::chrono::steady_clock::now() + d); }
    int requests = 0;

private:
    const Transport& t_;
    const CancellationToken& ct_;
    const std::function<void(int)>& waiting_;
    std::chrono::steady_clock::time_point next_{};
};

// The wait a rate-limited reply asks for: Retry-After, else ListenBrainz's reset, else a backoff (at least 10 s).
std::chrono::milliseconds rateWait(const HttpResponse& r, int attempt) {
    int s = retryAfterSeconds(r, unixNow());
    if (s < 0) s = headerInt(r, "X-RateLimit-Reset-In", -1);
    if (s >= 0) return std::chrono::milliseconds(std::clamp(s, 1, 300) * 1000LL);
    return std::max(backoff(attempt), std::chrono::milliseconds(10'000));
}

struct Failure {
    FetchError error = FetchError::None;
    std::string message;
};

std::string httpStatus(const HttpResponse& r) { return "HTTP " + std::to_string(r.statusCode); }

// ---- Last.fm ----------------------------------------------------------------------------------------------------

Failure lastfmFailure(int error, const std::string& message, int http) {
    Failure f;
    f.message = message.empty() ? "HTTP " + std::to_string(http) : message;
    switch (error) {
    case 6: f.error = FetchError::NotFound; break;      // "User not found"
    case 17: f.error = FetchError::Private; break;      // "Login: User required to be logged in" (private profile)
    case 10:
    case 26: f.error = FetchError::BadKey; break;
    case 29: f.error = FetchError::RateLimited; break;
    default: f.error = http == 0 ? FetchError::Network : FetchError::Server; break;
    }
    return f;
}

bool lastfmPage(Polite& net, const std::string& url, LastfmPage& page, Failure& failure) {
    const Headers headers{{"User-Agent", kUserAgent}, {"Accept", "application/json"}};
    for (int attempt = 0;; ++attempt) {
        HttpResponse r;
        try {
            r = net.get(url, headers);
        } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            if (attempt >= kMaxRetries) {
                failure = {FetchError::Network, e.what()};
                return false;
            }
            net.wait(backoff(attempt), false);
            continue;
        }
        page = parseLastfmPage(r.body);
        if (page.error == 0 && r.isSuccessStatusCode()) return true;
        const bool limited = r.statusCode == 429 || page.error == 29;
        const bool temporary = r.statusCode >= 500 || page.error == 8 || page.error == 11 || page.error == 16 ||
                               (page.error == -1 && !r.isSuccessStatusCode() && r.statusCode != 404 && r.statusCode != 403) ||
                               (page.error == -1 && r.isSuccessStatusCode());
        if ((limited || temporary) && attempt < kMaxRetries) {
            ST_LOG_WARN(kTag, "last.fm history: {} (error {}), retrying", httpStatus(r), page.error);
            net.wait(limited ? rateWait(r, attempt) : backoff(attempt), limited);
            continue;
        }
        failure = lastfmFailure(page.error == -1 ? 0 : page.error, page.message, r.statusCode);
        if (page.error == -1) failure = {r.statusCode == 404 ? FetchError::NotFound : FetchError::Server, httpStatus(r)};
        return false;
    }
}

FetchResult fetchLastfm(const FetchRequest& q, const Transport& t, const Sink& sink, const std::function<void(int64_t)>& checkpoint,
                        const std::function<void(const FetchStatus&)>& status, const CancellationToken& ct) {
    FetchResult res;
    res.next = q.since;
    if (q.apiKey.empty()) {
        res.error = FetchError::NoKey;
        res.message = "no API key";
        return res;
    }
    FetchStatus st;
    const std::function<void(int)> waiting = [&](int s) {
        st.waitingS = s;
        if (status) status(st);
    };
    Polite net(t, ct, waiting);
    const int64_t to = q.now > 0 ? q.now : unixNow();
    const int64_t from = q.since > 0 ? std::max<int64_t>(1, q.since - kResumeOverlapS) : 0;
    auto url = [&](int page) {
        std::string u = std::string(kLastfmApi) + "?method=user.getrecenttracks&user=" + http::urlEncode(q.user) +
                        "&api_key=" + http::urlEncode(q.apiKey) + "&format=json&limit=" + std::to_string(kLastfmPageSize) +
                        "&page=" + std::to_string(page) + "&to=" + std::to_string(to);
        if (from > 0) u += "&from=" + std::to_string(from);
        return u;
    };
    Failure failure;
    auto finish = [&](FetchError e, std::string message) {
        res.error = e;
        res.message = std::move(message);
        res.requests = net.requests;
        return res;
    };
    try {
        // The newest page first: it says how many pages the range has (and is kept for the end).
        LastfmPage newest;
        if (!lastfmPage(net, url(1), newest, failure)) return finish(failure.error, failure.message);
        const int pages = std::max(1, newest.totalPages);
        st.total = static_cast<size_t>(std::max<int64_t>(0, newest.total));
        if (status) status(st);
        auto hand = [&](LastfmPage& page) {
            int64_t newestTs = 0;
            for (auto it = page.rows.rbegin(); it != page.rows.rend(); ++it) {
                newestTs = std::max(newestTs, it->startedAt);
                sink(std::move(*it));
                ++res.rows;
            }
            page.rows = {};
            if (newestTs > res.next) res.next = newestTs;
            if (checkpoint) checkpoint(res.next);
            st.fetched = res.rows;
            st.fraction = st.total ? std::min(1.0, static_cast<double>(st.fetched) / static_cast<double>(st.total)) : 0.0;
            if (status) status(st);
        };
        for (int p = pages; p >= 2; --p) {
            LastfmPage page;
            if (!lastfmPage(net, url(p), page, failure)) return finish(failure.error, failure.message);
            hand(page);
        }
        hand(newest);
        st.fraction = 1;
        if (status) status(st);
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        return finish(FetchError::Cancelled, "cancelled");
    }
    res.requests = net.requests;
    return res;
}

// ---- ListenBrainz -----------------------------------------------------------------------------------------------

bool lbGet(Polite& net, const std::string& url, const Headers& headers, HttpResponse& out, Failure& failure) {
    for (int attempt = 0;; ++attempt) {
        try {
            out = net.get(url, headers);
        } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
            throw;
        } catch (const std::exception& e) {
            if (attempt >= kMaxRetries) {
                failure = {FetchError::Network, e.what()};
                return false;
            }
            net.wait(backoff(attempt), false);
            continue;
        }
        // X-RateLimit-Remaining 0: the next request waits for the window to reset.
        if (headerInt(out, "X-RateLimit-Remaining", -1) == 0)
            net.notBefore(std::chrono::milliseconds(std::clamp(headerInt(out, "X-RateLimit-Reset-In", 1), 1, 60) * 1000LL));
        if (out.isSuccessStatusCode()) return true;
        const bool limited = out.statusCode == 429;
        const bool temporary = out.statusCode >= 500;
        if ((limited || temporary) && attempt < kMaxRetries) {
            ST_LOG_WARN(kTag, "listenbrainz history: {}, retrying", httpStatus(out));
            net.wait(limited ? rateWait(out, attempt) : backoff(attempt), limited);
            continue;
        }
        const LbPage err = parseLbPage(out.body);
        failure.message = err.error.empty() ? httpStatus(out) : err.error;
        failure.error = out.statusCode == 404 ? FetchError::NotFound
                        : out.statusCode == 401 || out.statusCode == 403 ? FetchError::BadKey
                        : limited ? FetchError::RateLimited
                                  : FetchError::Server;
        return false;
    }
}

FetchResult fetchListenbrainz(const FetchRequest& q, const Transport& t, const Sink& sink,
                              const std::function<void(int64_t)>& checkpoint, const std::function<void(const FetchStatus&)>& status,
                              const CancellationToken& ct) {
    FetchResult res;
    res.next = q.since;
    const int64_t runStart = q.now > 0 ? q.now : unixNow();
    FetchStatus st;
    const std::function<void(int)> waiting = [&](int s) {
        st.waitingS = s;
        if (status) status(st);
    };
    Polite net(t, ct, waiting);
    Headers headers{{"User-Agent", kUserAgent}, {"Accept", "application/json"}};
    if (!q.token.empty()) headers.emplace_back("Authorization", "Token " + q.token);
    const std::string base = std::string(kLbApi) + "user/" + http::urlEncode(q.user) + "/";
    Failure failure;
    auto finish = [&](FetchError e, std::string message) {
        res.error = e;
        res.message = std::move(message);
        res.requests = net.requests;
        return res;
    };
    try {
        HttpResponse r;
        if (q.since <= 0) {   // the whole history: its size gives the progress a total
            if (!lbGet(net, base + "listen-count", headers, r, failure)) return finish(failure.error, failure.message);
            const json j = json::parse(r.body, nullptr, false);
            if (const json* p = member(j, "payload"))
                if (const json* c = member(*p, "count")) st.total = static_cast<size_t>(std::max<int64_t>(0, number(*c)));
            if (status) status(st);
        }
        int64_t minTs = q.since > 0 ? std::max<int64_t>(1, q.since - kResumeOverlapS) : 1;   // exclusive
        const int64_t firstTs = minTs;
        for (;;) {
            const std::string url = base + "listens?min_ts=" + std::to_string(minTs) + "&count=" + std::to_string(kLbPageSize);
            if (!lbGet(net, url, headers, r, failure)) return finish(failure.error, failure.message);
            LbPage page = parseLbPage(r.body);
            if (!page.ok) return finish(FetchError::Server, page.error.empty() ? "unexpected reply" : page.error);
            if (page.listens == 0) break;
            // Newest first on the page: hand them out oldest first.
            for (auto it = page.rows.rbegin(); it != page.rows.rend(); ++it) {
                sink(std::move(*it));
                ++res.rows;
            }
            const int64_t top = page.newestOnPage;
            // The cursor never passes the run's start: a listen stamped in the future (a client's wrong clock) would
            // make the next runs skip everything until then.
            if (std::min(top, runStart) > res.next) res.next = std::min(top, runStart);
            if (checkpoint) checkpoint(res.next);
            st.fetched = res.rows;
            if (st.total) st.fraction = std::min(1.0, static_cast<double>(st.fetched) / static_cast<double>(st.total));
            else if (page.latest > firstTs) st.fraction = std::clamp(static_cast<double>(top - firstTs) / static_cast<double>(page.latest - firstTs), 0.0, 1.0);
            if (status) status(st);
            if (page.listens < static_cast<size_t>(kLbPageSize) && top >= page.latest) break;   // reached the newest
            // Listens of the page's last second may continue on the next page: ask from just before it (the repeats are
            // duplicates). A page of one second only moves on.
            int64_t next = top - 1;
            if (next <= minTs) next = top;
            if (next <= minTs) break;
            minTs = next;
        }
        st.fraction = 1;
        if (status) status(st);
    } catch (const YoutubeExplode::Exceptions::OperationCanceledException&) {
        return finish(FetchError::Cancelled, "cancelled");
    }
    res.requests = net.requests;
    return res;
}

} // namespace

FetchResult fetch(const FetchRequest& request, const Transport& transport, const Sink& sink,
                  const std::function<void(int64_t next)>& checkpoint, const std::function<void(const FetchStatus&)>& status,
                  const CancellationToken& ct) {
    FetchResult r = request.service == Service::LastFm ? fetchLastfm(request, transport, sink, checkpoint, status, ct)
                                                       : fetchListenbrainz(request, transport, sink, checkpoint, status, ct);
    ST_LOG_INFO(kTag, "{} history: {} scrobbles in {} requests, cursor {}{}", importSourceName(sourceOf(request.service)), r.rows,
                r.requests, r.next, r.error == FetchError::None ? std::string() : " (stopped: " + r.message + ")");
    return r;
}

JobOutcome runJob(const ListenStats::ImportBase& base, const Job& job, const Transport& transport,
                  const std::function<void(const JobProgress&)>& progress, const std::atomic<bool>& cancel,
                  const CancellationToken& ct) {
    JobOutcome out;
    ListenStats::ImportBatch batch;
    JobProgress pr;
    const bool api = job.files.empty();
    bool addedAny = false, wroteAny = false, failed = false;
    int64_t cursor = 0;   // API: everything the account scrobbled before it is in `batch` or committed
    auto report = [&] {
        if (progress) progress(pr);
    };
    auto commit = [&](bool last) {
        if (failed) return;
        if (api && cursor > 0) batch.cursors = {{job.request.user, cursor}};
        if (batch.empty() && batch.cursors.empty()) {
            if (!last) return;
            if (!addedAny && !wroteAny) {   // nothing at all: no need to read the history again
                out.result = {};
                out.result.ok = true;
                out.result.token = base.token;
                out.result.source = base.source;
                out.result.file = base.importedFile;
                return;
            }
        }
        batch.install = last && addedAny;
        pr.merging = true;
        report();
        ListenStats::ImportResult r = ListenStats::buildImport(base, std::move(batch));
        batch = {};
        ++out.batches;
        pr.merging = false;
        report();
        out.rows += r.rows;
        out.added += r.added;
        out.duplicates += r.duplicates;
        out.tooShort += r.tooShort;
        addedAny = addedAny || r.added > 0;
        wroteAny = wroteAny || r.wrote;
        if (!r.ok) {
            failed = true;
            out.error = r.error;
        }
        if (last || failed) {
            r.wrote = wroteAny;   // a superseded or failed import: the store rewrites what it holds
            out.result = std::move(r);
        }
    };
    const Sink sink = [&](Row&& row) { batch.add(std::move(row)); };
    if (api) {
        const auto checkpoint = [&](int64_t next) {
            cursor = next;
            if (batch.rows.size() >= job.batchRows) commit(false);
        };
        const auto status = [&](const FetchStatus& s) {
            pr.rows = s.fetched;
            pr.total = s.total;
            pr.fraction = s.fraction;
            pr.waitingS = s.waitingS;
            report();
        };
        out.fetch = fetch(job.request, transport, sink, checkpoint, status, ct);
        out.cancelled = out.fetch.error == FetchError::Cancelled;
    } else {
        const Sink batched = [&](Row&& row) {
            batch.add(std::move(row));
            if (batch.rows.size() >= job.batchRows) commit(false);
        };
        const auto fileProgress = [&](int done, int total, size_t rows) {
            pr.rows = rows;
            pr.fraction = total > 0 ? static_cast<double>(done) / total : 0.0;
            report();
        };
        out.files = readFiles(job.service, job.files, batched, fileProgress, &cancel);
        out.cancelled = out.files.cancelled;
    }
    commit(true);
    ST_LOG_INFO(kTag, "import ({}) done: {} rows in {} batches, {} new, {} already known{}", importSourceName(base.source), out.rows,
                out.batches, out.added, out.duplicates, out.cancelled ? " (cancelled)" : "");
    return out;
}

} // namespace st::app::scrobbles
