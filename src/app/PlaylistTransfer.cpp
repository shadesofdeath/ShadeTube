// Playlist import / export flows (see PlaylistTransfer.h; the file formats are app/PlaylistIO).
#include "app/PlaylistTransfer.h"

#include "app/AppContext.h"
#include "app/CatalogMatch.h"
#include "app/Downloads.h"
#include "app/DownloadSync.h"
#include "app/DroppedFiles.h"
#include "app/InternetRadio.h"
#include "app/Links.h"
#include "app/LocalLibrary.h"
#include "app/PlaylistEditing.h"
#include "app/PlaylistIO.h"
#include "app/Router.h"
#include "app/ServiceImport.h"
#include "app/SettingsWidgets.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/Http.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "musicbrainz/MusicBrainz.h"
#include "player/Player.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"
#include "ui/TextBox.h"
#include "ui/Window.h"
#include "youtube/MatchService.h"
#include "ytmusic/YtMusic.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace st::app::transfer {

namespace {

using catalog::Track;
using Microsoft::WRL::ComPtr;
using CT = YoutubeExplode::CancellationToken;
namespace fs = std::filesystem;
namespace pio = playlistio;

Lifetime g_life;   // work of this module dies with the app
int g_reading = 0;  // imports being read

// ---- Fetching a whole list ------------------------------------------------------------------------------------------

// Worker thread: every page of a Spotify playlist / Liked Songs (a short pause between pages: rate limits).
std::vector<Track> spotifyTracks(spotify::Api* api, const std::string& uri, const CT& ct) {
    std::vector<Track> out;
    int offset = 0;
    for (int page = 0; page < 200; ++page) {   // at most 20 000 rows
        ct.throwIfCancellationRequested();
        auto p = api->playlistTracks(uri, offset, 100, ct);
        const int next = p.nextOffset >= 0 ? p.nextOffset : offset + static_cast<int>(p.items.size());
        for (auto& t : p.items) out.push_back(std::move(t));
        if (next <= offset || next >= p.total) break;
        offset = next;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return out;
}

// Songs only: no radio stations or podcast episodes in a playlist file.
std::vector<Track> songsOnly(std::vector<Track> tracks) {
    std::erase_if(tracks, [](const Track& t) { return t.id.empty() || radio::isStationId(t.id) || catalog::isPodcastId(t.id); });
    return tracks;
}

// The export rows: each song with its local file (a download, a local / dropped file) and its matched video.
std::vector<pio::Row> rowsFor(const std::vector<Track>& tracks) {
    std::vector<pio::Row> rows;
    rows.reserve(tracks.size());
    auto* matcher = ctx().matcher;
    for (const auto& t : tracks) {
        pio::Row r;
        r.track = t;
        if (const auto* d = ctx().downloads.item(t.id); d && d->state == DlState::Done) r.filePath = d->filePath;
        if (r.filePath.empty() && t.id.rfind("local:", 0) == 0) {
            std::wstring p = LocalLibrary::get().pathFor(t.id);
            if (p.empty()) p = dropped::Store::get().pathFor(t.id);
            r.filePath = toUtf8(p);
        }
        if (matcher && t.id.rfind("local:", 0) != 0)
            if (const auto m = matcher->cachedMatch(t.id)) r.videoId = m->videoId;
        rows.push_back(std::move(r));
    }
    return rows;
}

// A file name from a list name: no characters Windows refuses, not empty.
std::wstring safeFileName(const std::string& name) {
    std::wstring w = toWide(name);
    for (auto& c : w)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    while (!w.empty() && (w.back() == L' ' || w.back() == L'.')) w.pop_back();
    if (w.empty()) w = tr(L"Çalma listesi");
    if (w.size() > 120) w.resize(120);
    return w;
}

// Save dialog owned by the main window: the file and the format (the type picked; a typed extension of another
// known format wins). Modal: call it posted, outside a widget event.
bool askSaveFile(const std::string& name, fs::path& file, pio::Format& format) {
    static UINT lastType = 1;   // M3U8 first, then whatever was picked last time
    ComPtr<IFileSaveDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return false;
    const std::wstring m3u = tr(L"M3U8 çalma listesi (VLC, foobar2000, Winamp…)"), csv = tr(L"CSV tablosu (Excel)"),
                       xspf = tr(L"XSPF çalma listesi"), json = tr(L"ShadeTube listesi (JSON)");
    const COMDLG_FILTERSPEC types[] = {{m3u.c_str(), L"*.m3u8"}, {csv.c_str(), L"*.csv"}, {xspf.c_str(), L"*.xspf"},
                                       {json.c_str(), L"*.json"}};
    dlg->SetFileTypes(4, types);
    dlg->SetFileTypeIndex(lastType);
    dlg->SetDefaultExtension(L"m3u8");
    dlg->SetFileName(safeFileName(name).c_str());
    dlg->SetTitle(tr(L"Çalma listesini dışa aktar"));
    dlg->SetOkButtonLabel(tr(L"Dışa aktar"));
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_OVERWRITEPROMPT | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    ComPtr<IShellItem> music;   // first time: Music (later Windows remembers the last folder)
    if (SUCCEEDED(SHGetKnownFolderItem(FOLDERID_Music, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&music))))
        dlg->SetDefaultFolder(music.Get());
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return false;
    UINT type = 1;
    dlg->GetFileTypeIndex(&type);
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return false;
    file = path;
    CoTaskMemFree(path);
    static constexpr pio::Format kFormats[] = {pio::Format::M3U, pio::Format::CSV, pio::Format::XSPF, pio::Format::JSON};
    format = kFormats[std::clamp<UINT>(type, 1, 4) - 1];
    if (const auto byExt = pio::formatFromPath(file)) format = *byExt;
    else file += toWide(pio::extension(format));
    lastType = std::clamp<UINT>(type, 1, 4);
    return true;
}

void showFileToast(const std::wstring& message, const fs::path& file) {
    ui::Toasts::show(ctx().window, message, ui::ToastKind::Success, tr(L"Dosyayı göster"), [file] {
        const std::wstring arg = L"/select,\"" + file.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL);
    });
}

// UI thread: the rows are ready -> the save dialog -> written on a worker.
void saveRows(std::string name, std::vector<Track> tracks) {
    tracks = songsOnly(std::move(tracks));
    if (tracks.empty()) {
        toast(tr(L"Dışa aktarılacak şarkı yok"));
        return;
    }
    auto rows = std::make_shared<std::vector<pio::Row>>(rowsFor(tracks));
    Dispatcher::post([name, rows] {
        fs::path file;
        pio::Format format = pio::Format::M3U;
        if (!askSaveFile(name, file, format)) return;
        async(
            Priority::High, g_life.ref(),
            [name, rows, file, format] {
                const std::string text = pio::write(format, name, *rows);
                std::ofstream out(file, std::ios::binary | std::ios::trunc);
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
                if (!out) throw std::runtime_error("write failed");
                return rows->size();
            },
            [file](Result<size_t> r) {
                if (!r) {
                    ST_LOG_WARN("transfer", "export to {} failed: {}", toUtf8(file.wstring()), r.errorMessage());
                    toast(tr(L"Liste dışa aktarılamadı"), true);
                    return;
                }
                ST_LOG_INFO("transfer", "exported {} song(s) to {}", *r, toUtf8(file.wstring()));
                showFileToast(i18n::plural(L"{} şarkı dışa aktarıldı", static_cast<long long>(*r)), file);
            });
    });
}

// ---- Import ---------------------------------------------------------------------------------------------------------

// A Deezer / Apple Music / TIDAL list matched to the active catalog (importService): what the preview reports.
struct ServiceInfo {
    links::Service service = links::Service::None;
    std::wstring label;                 // "Deezer çalma listesi"
    std::wstring catalog;               // the catalog the songs were looked up in
    std::vector<char> found;            // parallel to Prepared::tracks: the catalog has the song
    int foundCount = 0, byIsrc = 0, skipped = 0, expected = 0;
    bool partial = false, interrupted = false;
    std::string description;            // the list's own, plain text
    std::vector<uint8_t> coverJpeg;     // its cover as the local playlist's (square, small), empty = none
};

struct Prepared {
    std::string name;
    std::vector<Track> tracks;
    std::vector<pio::Problem> unresolved;
    std::vector<local::Entry> localEntries;   // read local files, remembered so they keep playing
    std::optional<ServiceInfo> service;
};

std::string lowerPath(const std::wstring& p) {
    std::wstring w = fs::path(p).lexically_normal().wstring();
    CharLowerBuffW(w.data(), static_cast<DWORD>(w.size()));
    return toUtf8(w);
}

// A YouTube video as a song (title / artist from the video, its 16:9 thumbnails, exactly that video as the source).
Track videoTrack(const std::string& videoId, const std::string& title, const std::string& channel, int durationMs) {
    const auto song = links::videoSong(title, channel);
    Track t;
    t.id = "yt:" + videoId;
    t.videoId = videoId;
    t.name = song.title.empty() ? title : song.title;
    if (!song.artist.empty()) t.artists.push_back({"", song.artist});
    t.durationMs = durationMs;
    t.album.images.push_back({"https://i.ytimg.com/vi/" + videoId + "/mqdefault.jpg", 320, 180});
    return t;
}

// Worker thread: local files read, id-only songs named (Spotify / YouTube / MusicBrainz, at most kMaxLookups).
Prepared prepare(pio::Parsed parsed, spotify::Api* api, youtube::MatchService* matcher, const CT& ct) {
    Prepared out;
    out.name = parsed.name;
    out.unresolved = std::move(parsed.unresolved);
    // Local files: read once each through the local library's reader (tags, embedded covers).
    std::vector<std::wstring> paths;
    std::unordered_set<std::string> seen;
    for (const auto& e : parsed.entries)
        if (!e.localPath.empty() && seen.insert(lowerPath(toWide(e.localPath))).second) paths.push_back(toWide(e.localPath));
    std::unordered_map<std::string, const local::Entry*> byPath;
    dropped::Collected collected;
    if (!paths.empty()) {
        collected = dropped::collect(paths, dropped::Store::coverDir(), 20'000);
        for (const auto& le : collected.entries) byPath[lowerPath(le.path)] = &le;
    }
    int lookups = 0;
    for (auto& e : parsed.entries) {
        ct.throwIfCancellationRequested();
        if (!e.localPath.empty()) {
            if (const auto it = byPath.find(lowerPath(toWide(e.localPath))); it != byPath.end()) {
                Track t = local::toTrack(*it->second);
                t.addedAt = e.track.addedAt;
                out.tracks.push_back(std::move(t));
                continue;
            }
            // The file is gone (or unreadable): a song the list names ("Artist - Title") is still matched on YouTube.
            if (!e.track.artists.empty() && !e.track.name.empty()) {
                Track t = e.track;
                t.id = pio::importId(t.name, t.artists[0].name);
                out.tracks.push_back(std::move(t));
            } else {
                out.unresolved.push_back({e.line, e.localPath});
            }
            continue;
        }
        Track& t = e.track;
        if (!t.name.empty()) {
            out.tracks.push_back(std::move(t));
            continue;
        }
        // Only an id: ask its service for the title (a few hundred at most; the rest are reported).
        bool named = false;
        if (lookups < kMaxLookups) {
            ++lookups;
            try {
                if (t.id.rfind("spotify:track:", 0) == 0 && api) {
                    Track full = api->track(t.id, ct);
                    full.addedAt = t.addedAt;
                    t = std::move(full);
                    std::this_thread::sleep_for(std::chrono::milliseconds(120));
                } else if (t.id.rfind("yt:", 0) == 0 && matcher) {
                    const auto v = matcher->client().videos().get(t.videoId, ct);
                    t = videoTrack(t.videoId, v.title(), v.author().channelTitle(),
                                   v.duration() ? static_cast<int>(v.duration()->count()) : 0);
                } else if (links::isMbid(t.id)) {
                    Track full = mb::recording(t.id, ct);
                    full.addedAt = t.addedAt;
                    t = std::move(full);
                }
                named = !t.name.empty();
            } catch (const std::exception& ex) {
                ST_LOG_WARN("transfer", "{}: not found ({})", t.id, ex.what());
            }
        }
        if (named) out.tracks.push_back(std::move(t));
        else out.unresolved.push_back({e.line, t.id});
    }
    out.localEntries = std::move(collected.entries);
    return out;
}

// Adds the songs to the new Spotify playlist 100 at a time (the first batch went with the creation).
void addSpotifyBatches(std::string uri, std::shared_ptr<std::vector<std::string>> uris, size_t from, int added) {
    auto* sess = ctx().session;
    if (!sess || from >= uris->size()) {
        toast(i18n::plural(L"Spotify'da da oluşturuldu: {} şarkı", added));
        return;
    }
    const size_t to = std::min(uris->size(), from + 100);
    std::vector<std::string> batch(uris->begin() + static_cast<std::ptrdiff_t>(from), uris->begin() + static_cast<std::ptrdiff_t>(to));
    const int n = static_cast<int>(batch.size());
    sess->addToPlaylist(uri, std::move(batch), [uri, uris, to, added, n](bool ok) {
        if (!ok) {
            toast(i18n::plural(L"Spotify listesine yalnızca {} şarkı eklenebildi", added), true);
            return;
        }
        addSpotifyBatches(uri, uris, to, added + n);
    });
}

void createOnSpotify(const std::string& name, const std::vector<Track>& tracks) {
    auto* sess = ctx().session;
    if (!sess || !sess->loggedIn()) return;
    auto uris = std::make_shared<std::vector<std::string>>();
    std::unordered_set<std::string> seen;
    for (const auto& t : tracks)
        if (t.id.rfind("spotify:track:", 0) == 0 && seen.insert(t.id).second) uris->push_back(t.id);
    if (uris->empty()) return;
    std::vector<std::string> first(uris->begin(), uris->begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(100, uris->size())));
    const size_t firstCount = first.size();
    sess->createPlaylist(name, std::move(first), [uris, firstCount](const std::string& uri, int added) {
        if (uri.empty()) {
            toast(tr(L"Spotify listesi oluşturulamadı"), true);
            return;
        }
        addSpotifyBatches(uri, uris, firstCount, added);
    });
}

// The preview: name, what was found, the rows that name no song, and "Spotify'da da oluştur" (logged in, when some
// songs have Spotify ids). "İçe aktar" creates the local playlist and opens it.
void showPreview(std::shared_ptr<Prepared> prep, std::wstring source) {
    auto& tracks = prep->tracks;
    if (tracks.empty()) {
        toast(prep->unresolved.empty() ? tr(L"Listede şarkı bulunamadı")
                                       : i18n::plural(L"Listede şarkı bulunamadı; {} satır okunamadı",
                                                      static_cast<long long>(prep->unresolved.size())),
              true);
        return;
    }
    auto* d = ui::Dialog::open(ctx().window, tr(L"Çalma listesini içe aktar"), source, 560);
    if (!d) return;
    auto* body = d->body();
    auto* nameBox = body->add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Çalma listesi adı"));
    nameBox->setText(toWide(prep->name.empty() ? toUtf8(tr(L"İçe aktarılan liste")) : prep->name));
    int spotifyCount = 0, localCount = 0, ytCount = 0;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const auto& t = tracks[i];
        if (prep->service && !prep->service->found[i]) continue;   // songs known by name only: counted below
        if (t.id.rfind("spotify:track:", 0) == 0) ++spotifyCount;
        else if (t.id.rfind("local:", 0) == 0) ++localCount;
        else if (t.id.rfind("yt:", 0) == 0) ++ytCount;
    }
    constexpr size_t kShown = 6;
    ui::Toggle* keepMissing = nullptr;
    if (const auto& svcInfo = prep->service) {
        // "<n> songs · YouTube Music: <m> matched (<k> exactly by ISRC) · <x> not found", then the songs not found.
        const int missing = static_cast<int>(tracks.size()) - svcInfo->foundCount;
        std::wstring summary = i18n::plural(L"{} şarkı okundu", static_cast<long long>(tracks.size())) + L" · " +
                               i18n::format(tr(L"{}: {} eşleşti"), {svcInfo->catalog, std::to_wstring(svcInfo->foundCount)});
        if (svcInfo->byIsrc > 0) summary += L" (" + i18n::plural(L"{} tanesi ISRC ile birebir", svcInfo->byIsrc) + L")";
        if (missing > 0) summary += L" · " + i18n::plural(L"{} bulunamadı", missing);
        auto* sum = body->add<ui::Label>(summary, gfx::type::secondary, ui::Tone::Secondary);
        sum->setWrap(true);
        std::vector<std::wstring> notes;
        if (svcInfo->partial)
            notes.push_back(i18n::format(tr(L"{} bu listenin yalnızca ilk {} şarkısını verdi; liste daha uzun olabilir."),
                                         {svc::serviceName(svcInfo->service), std::to_wstring(tracks.size())}));
        if (svcInfo->skipped > 0)
            notes.push_back(i18n::plural(L"{} öğe atlandı: video ya da serviste artık olmayan şarkı.", svcInfo->skipped));
        if (svcInfo->interrupted)
            notes.push_back(tr(L"Eşleştirme bağlantı sorunları yüzünden yarıda kaldı; kalan şarkılar adlarıyla eklenir."));
        for (const auto& n : notes) body->add<ui::Label>(n, gfx::type::caption, ui::Tone::Tertiary)->setWrap(true, 3);
        size_t shown = 0;
        for (size_t i = 0; i < tracks.size() && shown < kShown; ++i) {
            if (svcInfo->found[i]) continue;
            ++shown;
            const auto& t = tracks[i];
            const std::string line = t.artistLine().empty() ? t.name : t.artistLine() + " \xE2\x80\x94 " + t.name;
            body->add<ui::Label>(std::to_wstring(i + 1) + L". " + toWide(line), gfx::type::caption, ui::Tone::Tertiary)->setWrap(true, 2);
        }
        if (missing > static_cast<int>(kShown))
            body->add<ui::Label>(i18n::plural(L"ve {} şarkı daha", missing - static_cast<long long>(kShown)), gfx::type::caption,
                                 ui::Tone::Tertiary);
        if (missing > 0) {
            auto* row = body->add<SettingRow>(tr(L"Bulunamayanları da ekle"),
                                              tr(L"Adlarıyla eklenirler; çalınırken YouTube'da adlarıyla aranırlar."));
            keepMissing = row->control<ui::Toggle>(40.f);
            keepMissing->setOn(true, false);
        }
    } else {
        std::wstring summary = i18n::plural(L"{} şarkı bulundu", static_cast<long long>(tracks.size()));
        if (localCount > 0) summary += L" · " + i18n::plural(L"{} yerel dosya", localCount);
        if (ytCount > 0) summary += L" · " + i18n::plural(L"{} YouTube videosu", ytCount);
        if (!prep->unresolved.empty())
            summary += L" · " + i18n::plural(L"{} satır okunamadı", static_cast<long long>(prep->unresolved.size()));
        auto* sum = body->add<ui::Label>(summary, gfx::type::secondary, ui::Tone::Secondary);
        sum->setWrap(true);
        for (size_t i = 0; i < prep->unresolved.size() && i < kShown; ++i) {
            const auto& u = prep->unresolved[i];
            auto* l = body->add<ui::Label>(i18n::format(tr(L"Satır {}: {}"), {std::to_wstring(u.line), toWide(u.text)}),
                                           gfx::type::caption, ui::Tone::Tertiary);
            l->setWrap(true, 2);
        }
        if (prep->unresolved.size() > kShown)
            body->add<ui::Label>(i18n::plural(L"ve {} satır daha", static_cast<long long>(prep->unresolved.size() - kShown)),
                                 gfx::type::caption, ui::Tone::Tertiary);
    }
    ui::Toggle* spotify = nullptr;
    if (source::loggedIn() && spotifyCount > 0) {
        auto* row = body->add<SettingRow>(tr(L"Spotify'da da oluştur"),
                                          i18n::plural(L"{} Spotify şarkısıyla hesabında da bir liste oluşturulur.",
                                                       spotifyCount));
        spotify = row->control<ui::Toggle>(40.f);
    }
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"İçe aktar"), ui::ButtonKind::Primary, [prep, nameBox, spotify, keepMissing] {
        std::wstring name = nameBox->text();
        if (name.empty()) name = tr(L"İçe aktarılan liste");
        if (!prep->localEntries.empty()) dropped::Store::get().remember(prep->localEntries);   // they keep playing
        std::vector<Track> songs = prep->tracks;
        if (prep->service && keepMissing && !keepMissing->on()) {
            songs.clear();
            for (size_t i = 0; i < prep->tracks.size(); ++i)
                if (prep->service->found[i]) songs.push_back(prep->tracks[i]);
        }
        auto& lib = ctx().library;
        const std::string id = lib.createPlaylist(name);
        const int added = lib.addToPlaylist(id, songs);
        if (prep->service) {
            if (!prep->service->description.empty()) lib.setPlaylistDescription(id, toWide(prep->service->description));
            if (!prep->service->coverJpeg.empty()) lib.setPlaylistCover(id, &prep->service->coverJpeg);
        }
        ST_LOG_INFO("transfer", "imported {} song(s) into {}", added, id);
        if (spotify && spotify->on()) createOnSpotify(toUtf8(name), songs);
        toast(i18n::plural(L"{} şarkı içe aktarıldı", added));
        const std::string open = id;
        Dispatcher::post([open] { ctx().router->navigate({RouteKind::Playlist, open}); });
    });
    nameBox->focus();
    nameBox->selectAll();
}

void runImport(std::function<pio::Parsed(const CT&)> read, std::wstring source) {
    spotify::Api* api = source::activeApi();
    auto* matcher = ctx().matcher;
    ++g_reading;
    // Most files read at once; a long list (or a YouTube playlist) says so when it isn't done within a second.
    SetTimer(nullptr, 0, 1000, [](HWND, UINT, UINT_PTR id, DWORD) {
        KillTimer(nullptr, id);
        if (g_reading > 0) toast(tr(L"Liste okunuyor…"));
    });
    async(
        Priority::High, g_life.ref(),
        [read = std::move(read), api, matcher] {
            const CT ct;
            return std::make_shared<Prepared>(prepare(read(ct), api, matcher, ct));
        },
        [source](Result<std::shared_ptr<Prepared>> r) {
            --g_reading;
            if (!r) {
                ST_LOG_WARN("transfer", "import failed: {}", r.errorMessage());
                toast(tr(L"Liste okunamadı"), true);
                return;
            }
            showPreview(*r, source);
        });
}

void pickImportFile() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const std::wstring lists = tr(L"Çalma listeleri (M3U, CSV, XSPF, JSON)"), all = tr(L"Tüm dosyalar");
    const COMDLG_FILTERSPEC types[] = {{lists.c_str(), L"*.m3u;*.m3u8;*.csv;*.tsv;*.xspf;*.json;*.txt"}, {all.c_str(), L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(tr(L"Çalma listesi içe aktar"));
    dlg->SetOkButtonLabel(tr(L"İçe aktar"));
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return;
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return;
    const std::wstring file = path;
    CoTaskMemFree(path);
    importFile(file);
}

// ---- Deezer / Apple Music / TIDAL -------------------------------------------------------------------------------------

catmatch::Catalog activeCatalog() {
    switch (source::active()) {
    case source::Catalog::Spotify: return catmatch::Catalog::Spotify;
    case source::Catalog::YouTubeMusic: return catmatch::Catalog::YouTubeMusic;
    case source::Catalog::MusicBrainz: break;
    }
    return catmatch::Catalog::MusicBrainz;
}

std::wstring kindLabel(links::Service s, svc::Kind k) {
    const std::wstring name = svc::serviceName(s);
    switch (k) {
    case svc::Kind::Playlist: return i18n::format(tr(L"{} çalma listesi"), {name});
    case svc::Kind::Album: return i18n::format(tr(L"{} albümü"), {name});
    case svc::Kind::Track: return i18n::format(tr(L"{} şarkısı"), {name});
    }
    return name;
}

// Why a list could not be read, for a toast.
std::wstring errorText(const std::exception_ptr& error, links::Service s, svc::Kind k) {
    const std::wstring name = svc::serviceName(s);
    try {
        std::rethrow_exception(error);
    } catch (const svc::Error& e) {
        switch (e.code) {
        case svc::Error::NotFound:
            return k == svc::Kind::Playlist ? tr(L"Bu çalma listesi açılamadı: gizli, silinmiş ya da bu ülkede kapalı olabilir.")
                                            : tr(L"Bu albüm ya da şarkı bulunamadı: kaldırılmış ya da bu ülkede kapalı olabilir.");
        case svc::Error::Private: return tr(L"Bu liste gizli: sahibi herkese açık yapmadıkça okunamıyor.");
        case svc::Error::Unavailable: return tr(L"Bu liste bu ülkede açılamıyor.");
        case svc::Error::Blocked:
            return i18n::format(tr(L"{} şu anda hesapsız okumaya izin vermiyor. Biraz sonra yeniden dene."), {name});
        case svc::Error::Network: return i18n::format(tr(L"{} yanıt vermiyor. Bağlantını kontrol et."), {name});
        case svc::Error::Format: break;
        }
    } catch (...) {
    }
    return i18n::format(tr(L"{} yanıtı okunamadı."), {name});
}

// A running service import, shared by the worker and the progress dialog.
struct ImportJob {
    YoutubeExplode::CancellationTokenSource cts;
    links::Service service = links::Service::None;
    svc::Kind kind = svc::Kind::Playlist;
    std::wstring catalog;
    std::atomic<int> phase{0};   // 0 reading the service, 1 matching in the catalog
    std::atomic<int> read{0}, expected{0}, total{0};
    std::atomic<bool> finished{false};
    catmatch::Counters counters;
    ui::Dialog* dialog = nullptr;   // UI thread; null once it closed
    bool cancelled = false;         // UI thread: "Vazgeç" / Escape
};

// The progress dialog's content: the step, songs done / all and a 2 px progress line (Stats' import banner's look);
// it repaints itself ~7 times a second until the job is done.
class ImportProgressView : public ui::Widget {
public:
    explicit ImportProgressView(std::shared_ptr<ImportJob> job)
        : job_(std::move(job)), text_({}, gfx::type::body), count_({}, gfx::type::monoMeta), detail_({}, gfx::type::caption) {
        hitTestVisible = false;
    }
    // Gone with its dialog: after a close (the job is done or was cancelled then) or when the app quits with the import
    // still running, which must not keep the process alive until the matching ends.
    ~ImportProgressView() override {
        job_->dialog = nullptr;
        if (!job_->finished) {
            job_->cancelled = true;
            job_->cts.cancel();
        }
    }
    float preferredHeight(float) override { return 58; }
    void paint(gfx::Canvas& c) override {
        const gfx::Rect r = rect();
        const auto& col = gfx::colors();
        const auto& j = *job_;
        std::wstring title, count, detail;
        float f = 0;
        if (j.phase == 0) {
            title = i18n::format(tr(L"{} okunuyor…"), {svc::serviceName(j.service)});
            const int n = j.read, e = j.expected;
            count = e > 0 ? std::to_wstring(n) + L" / " + std::to_wstring(e) : n > 0 ? std::to_wstring(n) : std::wstring{};
            f = e > 0 ? std::min(1.f, static_cast<float>(n) / static_cast<float>(e)) : 0.f;
            detail = tr(L"Şarkılar, adları ve ISRC kodlarıyla okunuyor.");
        } else {
            title = i18n::format(tr(L"{} kataloğunda eşleştiriliyor…"), {j.catalog});
            const int done = j.counters.done, total = std::max(1, j.total.load());
            count = std::to_wstring(std::min(done, total)) + L" / " + std::to_wstring(total);
            f = std::min(1.f, static_cast<float>(done) / static_cast<float>(total));
            detail = i18n::plural(L"{} şarkı eşleşti", j.counters.found.load());
            if (j.counters.byIsrc > 0) detail += L" · " + i18n::plural(L"{} tanesi ISRC ile birebir", j.counters.byIsrc.load());
        }
        text_.setText(title);
        count_.setText(count);
        detail_.setText(detail);
        const float cw = std::ceil(count_.measure().w);
        c.text(text_, {r.x, r.y, std::max(0.f, r.w - cw - 16), 22}, col.fgPrimary, gfx::VAlign::Center);
        c.text(count_, {r.right() - cw, r.y, cw, 22}, col.fgTertiary, gfx::VAlign::Center);
        c.text(detail_, {r.x, r.y + 24, r.w, 18}, col.fgSecondary, gfx::VAlign::Center);
        const gfx::Rect track{r.x, r.bottom() - 4, r.w, 2};
        c.fillRect(track, col.hairSubtle);
        c.fillRect({track.x, track.y, std::round(track.w * f), track.h}, gfx::accent().base);
        if (!j.finished)
            if (auto* w = window()) w->invalidateAfter(150);
    }

private:
    std::shared_ptr<ImportJob> job_;
    gfx::Text text_, count_, detail_;
};

// Reads the list (unless `preRead` has it), matches its songs in the active catalog behind a progress dialog with
// "Vazgeç", fetches its cover, then shows the preview.
void importService(const svc::Ref& ref, std::shared_ptr<svc::List> preRead) {
    auto job = std::make_shared<ImportJob>();
    job->service = ref.service;
    job->kind = ref.kind;
    const catmatch::Catalog catalog = activeCatalog();
    job->catalog = catmatch::catalogName(catalog);
    spotify::Api* api = catalog == catmatch::Catalog::Spotify ? source::activeApi() : nullptr;
    if (preRead) job->read = job->expected = static_cast<int>(preRead->songs.size());
    const std::wstring label = kindLabel(ref.service, ref.kind);
    auto* d = ui::Dialog::open(ctx().window, tr(L"Çalma listesi içe aktarılıyor"), label, 520);
    if (!d) return;
    job->dialog = d;
    d->body()->add<ImportProgressView>(job);
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    d->onClosed = [job] {
        job->dialog = nullptr;
        if (!job->finished) {
            job->cancelled = true;
            job->cts.cancel();
            ST_LOG_INFO("transfer", "service import cancelled");
        }
    };
    async(
        Priority::High, g_life.ref(),
        [job, ref, preRead, catalog, api, label] {
            const CT ct = job->cts.token();
            svc::List list = preRead ? *preRead
                                     : svc::read(ref, svc::liveFetch(), [job](int r, int e) {
                                           job->read = r;
                                           job->expected = e;
                                       }, ct);
            job->total = static_cast<int>(list.songs.size());
            job->phase = 1;
            bool interrupted = false;
            auto results = catmatch::matchAll(list.songs, catalog, api, job->counters, ct, &interrupted);
            auto prep = std::make_shared<Prepared>();
            prep->name = list.title;
            ServiceInfo info;
            info.service = ref.service;
            info.label = label;
            info.catalog = catmatch::catalogName(catalog);
            info.foundCount = job->counters.found;
            info.byIsrc = job->counters.byIsrc;
            info.skipped = list.skipped;
            info.expected = list.expected;
            info.partial = list.partial;
            info.interrupted = interrupted;
            info.description = list.description;
            for (auto& r : results) {
                info.found.push_back(r.found ? 1 : 0);
                prep->tracks.push_back(std::move(r.track));
            }
            // The list's cover as the local playlist's (best effort; an album's cover is its songs' anyway).
            if (!list.coverUrl.empty() && ref.kind == svc::Kind::Playlist) {
                try {
                    const auto resp = http::get(list.coverUrl, {}, ct);
                    if (resp.isSuccessStatusCode() && !resp.body.empty())
                        info.coverJpeg = pledit::squareJpeg(reinterpret_cast<const uint8_t*>(resp.body.data()), resp.body.size()).jpeg;
                } catch (const std::exception& e) {
                    if (ct.isCancellationRequested()) throw;
                    ST_LOG_WARN("transfer", "cover of {} not fetched: {}", list.title, e.what());
                }
            }
            prep->service = std::move(info);
            return prep;
        },
        [job, label](Result<std::shared_ptr<Prepared>> r) {
            job->finished = true;
            if (job->dialog) job->dialog->close();
            if (job->cancelled) return;
            if (!r) {
                ST_LOG_WARN("transfer", "service import failed: {}", r.errorMessage());
                toast(errorText(r.error(), job->service, job->kind), true);
                return;
            }
            showPreview(*r, label);
        });
}

// A song link: its match in the active catalog plays (else the song by name: MatchService finds its video).
void playServiceTrack(const svc::Ref& ref) {
    const catmatch::Catalog catalog = activeCatalog();
    spotify::Api* api = catalog == catmatch::Catalog::Spotify ? source::activeApi() : nullptr;
    toast(tr(L"Bağlantı açılıyor…"));
    async(
        Priority::High, g_life.ref(),
        [ref, catalog, api] {
            const svc::List l = svc::read(ref, svc::liveFetch());
            catmatch::Counters c;
            auto results = catmatch::matchAll(l.songs, catalog, api, c, {});
            if (results.empty()) throw svc::Error(svc::Error::NotFound, "no song");
            return results.front();
        },
        [ref](Result<catmatch::Result> r) {
            if (!r) {
                ST_LOG_WARN("transfer", "service song not opened: {}", r.errorMessage());
                toast(errorText(r.error(), ref.service, ref.kind), true);
                return;
            }
            ST_LOG_INFO("transfer", "service song -> {} ({})", r->track.id, r->found ? "catalog" : "by name");
            if (auto* p = ctx().player) p->playContext({r->track}, 0, {"link", kindLabel(ref.service, ref.kind)});
        });
}

// An album link: the active catalog's album opens when it has it; otherwise its songs are imported like a playlist.
// `importAlways`: the import dialog asked for it (no album page).
void openServiceAlbum(const svc::Ref& ref, bool importAlways) {
    if (importAlways) return importService(ref, nullptr);
    const catmatch::Catalog catalog = activeCatalog();
    spotify::Api* api = catalog == catmatch::Catalog::Spotify ? source::activeApi() : nullptr;
    toast(tr(L"Bağlantı açılıyor…"));
    struct Out {
        std::shared_ptr<svc::List> album;
        std::string catalogId;
    };
    async(
        Priority::High, g_life.ref(),
        [ref, catalog, api] {
            Out o;
            o.album = std::make_shared<svc::List>(svc::read(ref, svc::liveFetch()));
            try {
                o.catalogId = catmatch::findAlbum(*o.album, catalog, api, {});
            } catch (const std::exception& e) {
                ST_LOG_WARN("transfer", "album lookup failed: {}", e.what());
            }
            return o;
        },
        [ref](Result<Out> r) {
            if (!r) {
                ST_LOG_WARN("transfer", "service album not opened: {}", r.errorMessage());
                toast(errorText(r.error(), ref.service, ref.kind), true);
                return;
            }
            ST_LOG_INFO("transfer", "service album '{}' -> {}", r->album->title, r->catalogId.empty() ? "(import)" : r->catalogId);
            if (!r->catalogId.empty() && ctx().router) ctx().router->navigate({RouteKind::Album, r->catalogId});
            else importService(ref, r->album);
        });
}

void openServiceRef(const svc::Ref& ref, bool fromImportDialog) {
    switch (ref.kind) {
    case svc::Kind::Playlist: return importService(ref, nullptr);
    case svc::Kind::Album: return openServiceAlbum(ref, fromImportDialog);
    case svc::Kind::Track: return playServiceTrack(ref);
    }
}

void openServiceLinkImpl(const links::Link& link, bool fromImportDialog) {
    if (link.kind == links::Kind::ServiceShortLink) {
        toast(tr(L"Bağlantı açılıyor…"));
        const std::string url = link.id;
        async(Priority::High, g_life.ref(), [url] { return svc::resolveShortLink(url, svc::liveFetch()); },
              [fromImportDialog](Result<std::optional<svc::Ref>> r) {
                  if (!r || !*r) {
                      ST_LOG_WARN("transfer", "short link not resolved: {}", r ? "no list" : r.errorMessage());
                      toast(tr(L"Bu Deezer bağlantısının süresi dolmuş ya da bir liste, albüm veya şarkıya gitmiyor."), true);
                      return;
                  }
                  openServiceRef(**r, fromImportDialog);
              });
        return;
    }
    if (const auto ref = svc::ref(link)) openServiceRef(*ref, fromImportDialog);
}

} // namespace

// ---- Public API -----------------------------------------------------------------------------------------------------

void exportList(What what, const std::string& id, const std::string& name) {
    auto& lib = ctx().library;
    switch (what) {
    case What::Queue: {
        std::vector<Track> tracks;
        if (auto* p = ctx().player)
            for (const int i : p->order()) tracks.push_back(p->items()[static_cast<size_t>(i)]);
        return saveRows(name.empty() ? toUtf8(tr(L"Çalma sırası")) : name, std::move(tracks));
    }
    case What::Liked:
        if (id != sync::kSpotifyLikedId && (id == sync::kLocalLikedId || !source::loggedIn()))
            return saveRows(name, lib.liked());
        break;
    case What::Playlist:
        if (id.rfind("spotify:", 0) != 0 && !source::isYtmId(id)) {
            const auto* tracks = lib.playlistTracks(id);
            return saveRows(name, tracks ? *tracks : std::vector<Track>{});
        }
        break;
    case What::Album: break;
    }
    // Spotify / YouTube Music lists and albums: fetched in full on a worker.
    spotify::Api* api = source::activeApi();
    const bool ytmList = what == What::Playlist && source::isYtmId(id);
    if (what != What::Album && !ytmList && !api) {
        toast(tr(L"Spotify'a bağlı değilsin"), true);
        return;
    }
    const std::string uri = what == What::Liked ? std::string(spotify::Api::kLikedSongsUri) : id;
    toast(tr(L"Liste hazırlanıyor…"));
    async(
        Priority::High, g_life.ref(),
        [api, uri, what, ytmList] {
            const CT ct;
            if (ytmList) return ytm::playlistTracks(uri, 5000, ct);
            if (what == What::Album) {
                auto album = source::album(api, uri, ct);
                for (auto& t : album.tracks)
                    if (t.album.name.empty()) t.album = {album.id, album.name, album.images};
                return album.tracks;
            }
            return spotifyTracks(api, uri, ct);
        },
        [name](Result<std::vector<Track>> r) {
            if (!r) {
                ST_LOG_WARN("transfer", "export listing failed: {}", r.errorMessage());
                toast(tr(L"Liste alınamadı"), true);
                return;
            }
            saveRows(name, std::move(*r));
        });
}

ui::MenuItem exportMenuItem(What what, const std::string& id, const std::string& name) {
    return {tr(L"Dışa aktar…"), "share", L"", [what, id, name] { exportList(what, id, name); }};
}

void exportTracks(const std::string& name, std::vector<Track> tracks) { saveRows(name, std::move(tracks)); }

void importFromFile() { Dispatcher::post([] { pickImportFile(); }); }

void importFile(const std::wstring& path) {
    const fs::path file = path;
    runImport([file](const CT&) { return pio::parseFile(file); }, file.filename().wstring());
}

bool isPlaylistFile(const std::wstring& path) { return pio::formatFromPath(fs::path(path)).has_value(); }

void importFromLink(links::Service service) {
    Dispatcher::post([service] {
        // Every dialog takes any of the links; the service only picks the wording and the example.
        std::wstring title, text, example;
        const std::wstring catalog = catmatch::catalogName(activeCatalog());
        switch (service) {
        case links::Service::Deezer:
            title = tr(L"Deezer'dan içe aktar");
            example = L"https://www.deezer.com/playlist/…";
            break;
        case links::Service::AppleMusic:
            title = tr(L"Apple Music'ten içe aktar");
            example = L"https://music.apple.com/…/playlist/…";
            break;
        case links::Service::Tidal:
            title = tr(L"TIDAL'dan içe aktar");
            example = L"https://tidal.com/playlist/…";
            break;
        default: break;
        }
        if (title.empty()) {
            title = tr(L"YouTube listesinden içe aktar");
            text = tr(L"Bir YouTube ya da YouTube Music oynatma listesinin bağlantısını yapıştır. "
                      L"Videolar, tam o kayıtlardan çalan bir yerel listeye dönüşür.");
            example = L"https://www.youtube.com/playlist?list=…";
        } else {
            text = i18n::format(tr(L"Bir {} çalma listesinin ya da albümünün bağlantısını yapıştır. Şarkılar {} kataloğunda "
                                   L"eşleştirilir ve bir yerel listeye dönüşür; liste herkese açık olmalı."),
                                {svc::serviceName(service), catalog});
        }
        auto* d = ui::Dialog::open(ctx().window, title, text, 520);
        if (!d) return;
        auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, example);
        auto submit = [box] {
            const auto link = links::parse(box->text());
            switch (link.kind) {
            case links::Kind::YouTubePlaylist: return importYouTubePlaylist(link.id);
            case links::Kind::ServicePlaylist:
            case links::Kind::ServiceAlbum:
            case links::Kind::ServiceTrack:
            case links::Kind::ServiceShortLink:   // after this dialog closed (the progress dialog takes its place)
                Dispatcher::post([link] { openServiceLinkImpl(link, true); });
                return;
            default: break;
            }
            toast(tr(L"Bu bağlantı içe aktarılamıyor. YouTube, Deezer, Apple Music ve TIDAL çalma listesi ya da albüm "
                     L"bağlantıları desteklenir."),
                  true);
        };
        box->onSubmit = [d, submit](const std::wstring&) {
            submit();
            d->close();
        };
        d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
        d->addButton(tr(L"İçe aktar"), ui::ButtonKind::Primary, submit);
        box->focus();
    });
}

void importYouTubePlaylist(const std::string& playlistId) {
    auto* matcher = ctx().matcher;
    if (!matcher) return;
    runImport(
        [matcher, playlistId](const CT& ct) {
            pio::Parsed p;
            const auto& client = matcher->client();
            const auto meta = client.playlists().get(playlistId, ct);
            p.name = meta.title();
            int n = 0;
            for (const auto& v : client.playlists().getVideos(playlistId, 5000, ct)) {
                pio::Entry e;
                e.line = ++n;
                e.track = videoTrack(v.id().value(), v.title(), v.author().channelTitle(),
                                     v.duration() ? static_cast<int>(v.duration()->count()) : 0);
                p.entries.push_back(std::move(e));
            }
            return p;
        },
        tr(L"YouTube oynatma listesi"));
}

void importYtmPlaylist(const std::string& id) {
    runImport(
        [id](const CT& ct) {
            pio::Parsed p;
            p.name = ytm::playlist(id, ct).playlist.name;
            int n = 0;
            for (auto& t : ytm::playlistTracks(id, 5000, ct)) {   // named and pinned to their videos: no lookups
                pio::Entry e;
                e.line = ++n;
                e.track = std::move(t);
                p.entries.push_back(std::move(e));
            }
            return p;
        },
        tr(L"YouTube Music çalma listesi"));
}

void openServiceLink(const links::Link& link) { openServiceLinkImpl(link, false); }

std::vector<ui::MenuItem> importMenuItems() {
    return {{tr(L"Dosyadan içe aktar…"), "folder", L"", [] { importFromFile(); }},
            {tr(L"YouTube listesinden içe aktar…"), "youtube-source", L"", [] { importFromLink(links::Service::YouTube); }},
            {tr(L"Deezer'dan içe aktar…"), "link", L"", [] { importFromLink(links::Service::Deezer); }},
            {tr(L"Apple Music'ten içe aktar…"), "link", L"", [] { importFromLink(links::Service::AppleMusic); }},
            {tr(L"TIDAL'dan içe aktar…"), "link", L"", [] { importFromLink(links::Service::Tidal); }}};
}

void showImportMenu(gfx::Point windowPos) { ui::Menu::open(ctx().window, windowPos, importMenuItems()); }

} // namespace st::app::transfer
