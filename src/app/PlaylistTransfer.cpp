// Playlist import / export flows (see PlaylistTransfer.h; the file formats are app/PlaylistIO).
#include "app/PlaylistTransfer.h"

#include "app/AppContext.h"
#include "app/Downloads.h"
#include "app/DownloadSync.h"
#include "app/DroppedFiles.h"
#include "app/InternetRadio.h"
#include "app/Links.h"
#include "app/LocalLibrary.h"
#include "app/PlaylistIO.h"
#include "app/Router.h"
#include "app/SettingsWidgets.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "musicbrainz/MusicBrainz.h"
#include "player/Player.h"
#include "spotify/Session.h"
#include "spotify/SpotifyApi.h"
#include "ui/TextBox.h"
#include "ui/Window.h"
#include "youtube/MatchService.h"

#include <knownfolders.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
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

struct Prepared {
    std::string name;
    std::vector<Track> tracks;
    std::vector<pio::Problem> unresolved;
    std::vector<local::Entry> localEntries;   // read local files, remembered so they keep playing
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
    for (const auto& t : tracks) {
        if (t.id.rfind("spotify:track:", 0) == 0) ++spotifyCount;
        else if (t.id.rfind("local:", 0) == 0) ++localCount;
        else if (t.id.rfind("yt:", 0) == 0) ++ytCount;
    }
    std::wstring summary = i18n::plural(L"{} şarkı bulundu", static_cast<long long>(tracks.size()));
    if (localCount > 0) summary += L" · " + i18n::plural(L"{} yerel dosya", localCount);
    if (ytCount > 0) summary += L" · " + i18n::plural(L"{} YouTube videosu", ytCount);
    if (!prep->unresolved.empty())
        summary += L" · " + i18n::plural(L"{} satır okunamadı", static_cast<long long>(prep->unresolved.size()));
    auto* sum = body->add<ui::Label>(summary, gfx::type::secondary, ui::Tone::Secondary);
    sum->setWrap(true);
    constexpr size_t kShown = 6;
    for (size_t i = 0; i < prep->unresolved.size() && i < kShown; ++i) {
        const auto& u = prep->unresolved[i];
        auto* l = body->add<ui::Label>(i18n::format(tr(L"Satır {}: {}"), {std::to_wstring(u.line), toWide(u.text)}),
                                       gfx::type::caption, ui::Tone::Tertiary);
        l->setWrap(true, 2);
    }
    if (prep->unresolved.size() > kShown)
        body->add<ui::Label>(i18n::plural(L"ve {} satır daha", static_cast<long long>(prep->unresolved.size() - kShown)),
                             gfx::type::caption, ui::Tone::Tertiary);
    ui::Toggle* spotify = nullptr;
    if (source::loggedIn() && spotifyCount > 0) {
        auto* row = body->add<SettingRow>(tr(L"Spotify'da da oluştur"),
                                          i18n::plural(L"{} Spotify şarkısıyla hesabında da bir liste oluşturulur.",
                                                       spotifyCount));
        spotify = row->control<ui::Toggle>(40.f);
    }
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    d->addButton(tr(L"İçe aktar"), ui::ButtonKind::Primary, [prep, nameBox, spotify] {
        std::wstring name = nameBox->text();
        if (name.empty()) name = tr(L"İçe aktarılan liste");
        if (!prep->localEntries.empty()) dropped::Store::get().remember(prep->localEntries);   // they keep playing
        auto& lib = ctx().library;
        const std::string id = lib.createPlaylist(name);
        const int added = lib.addToPlaylist(id, prep->tracks);
        ST_LOG_INFO("transfer", "imported {} song(s) into {}", added, id);
        if (spotify && spotify->on()) createOnSpotify(toUtf8(name), prep->tracks);
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
        if (id.rfind("spotify:", 0) != 0) {
            const auto* tracks = lib.playlistTracks(id);
            return saveRows(name, tracks ? *tracks : std::vector<Track>{});
        }
        break;
    case What::Album: break;
    }
    // Spotify lists and albums: fetched in full on a worker.
    spotify::Api* api = source::activeApi();
    if (what != What::Album && !api) {
        toast(tr(L"Spotify'a bağlı değilsin"), true);
        return;
    }
    const std::string uri = what == What::Liked ? std::string(spotify::Api::kLikedSongsUri) : id;
    toast(tr(L"Liste hazırlanıyor…"));
    async(
        Priority::High, g_life.ref(),
        [api, uri, what] {
            const CT ct;
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

void importFromFile() { Dispatcher::post([] { pickImportFile(); }); }

void importFile(const std::wstring& path) {
    const fs::path file = path;
    runImport([file](const CT&) { return pio::parseFile(file); }, file.filename().wstring());
}

bool isPlaylistFile(const std::wstring& path) { return pio::formatFromPath(fs::path(path)).has_value(); }

void importFromLink() {
    Dispatcher::post([] {
        auto* d = ui::Dialog::open(ctx().window, tr(L"YouTube listesinden içe aktar"),
                                   tr(L"Bir YouTube ya da YouTube Music oynatma listesinin bağlantısını yapıştır. "
                                      L"Videolar, tam o kayıtlardan çalan bir yerel listeye dönüşür."),
                                   520);
        if (!d) return;
        auto* box = d->body()->add<ui::TextBox>(ui::TextBox::Look::Field, L"https://www.youtube.com/playlist?list=…");
        auto submit = [box] {
            const auto link = links::parse(box->text());
            if (link.kind != links::Kind::YouTubePlaylist) {
                toast(tr(L"Bu bir YouTube oynatma listesi bağlantısı değil"), true);
                return;
            }
            importYouTubePlaylist(link.id);
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

void showImportMenu(gfx::Point windowPos) {
    ui::Menu::open(ctx().window, windowPos,
                   {{tr(L"Dosyadan içe aktar…"), "folder", L"", [] { importFromFile(); }},
                    {tr(L"YouTube listesinden içe aktar…"), "youtube-source", L"", [] { importFromLink(); }}});
}

} // namespace st::app::transfer
