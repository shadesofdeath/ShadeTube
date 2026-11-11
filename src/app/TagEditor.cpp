// "Etiketleri düzenle…" (see TagEditor.h). UI thread; tag reads and writes on workers.
#include "app/TagEditor.h"

#include "app/AppContext.h"
#include "app/Components.h"
#include "app/DroppedFiles.h"
#include "app/LocalLibrary.h"
#include "app/PlaylistEditing.h"
#include "audio/TagWriter.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <objbase.h>

#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace st::app::tagedit {

namespace fs = std::filesystem;
namespace tags = audio::tags;
using catalog::Track;
using gfx::colors;
namespace type = gfx::type;

namespace {

Lifetime g_life;          // reads and writes outlive the dialog
bool g_busy = false;      // a read or a write is running

struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

// The picture shown in the open dialog (a temporary file gfx::ImageCache can load).
fs::path previewDir() {
    std::error_code ec;
    return fs::temp_directory_path(ec) / L"ShadeTube";
}

struct Target {
    std::wstring path;
    Track track;   // one track naming the file (for the library / queue updates)
};

// The tracks' files, each once.
std::vector<Target> targetsFor(const std::vector<Track>& tracks) {
    std::vector<Target> out;
    std::unordered_set<std::wstring> seen;
    for (const auto& t : tracks) {
        std::wstring path = fileFor(t);
        if (path.empty()) continue;
        std::wstring key = toLowerTr(path);
        if (seen.insert(std::move(key)).second) out.push_back({std::move(path), t});
    }
    return out;
}

// A field across the files: the shared value, or `mixed`.
struct Field {
    std::string value;
    bool mixed = false;
    bool first = true;
    void merge(const std::string& v) {
        if (first) {
            value = v;
            first = false;
        } else if (v != value) {
            mixed = true;
        }
    }
};

struct Loaded {
    std::vector<Target> targets;   // the files that were read (writable formats)
    int skipped = 0;               // files in another format, or unreadable
    Field title, artist, album, albumArtist, genre, track, disc, year;
    std::optional<tags::Picture> cover;   // the first file's
    bool coverMixed = false;
    std::vector<std::string> formats;
};

std::string number(int n) { return n > 0 ? std::to_string(n) : std::string(); }

// A "\\?\"-prefixed long path in its plain spelling (ids and the index use that one).
std::wstring plainPath(const std::wstring& p) {
    if (p.starts_with(LR"(\\?\UNC\)")) return LR"(\\)" + p.substr(8);
    if (p.starts_with(LR"(\\?\)")) return p.substr(4);
    return p;
}

// Worker: every file's tags, merged.
std::shared_ptr<Loaded> load(std::vector<Target> targets) {
    auto out = std::make_shared<Loaded>();
    bool firstCover = true;
    for (auto& t : targets) {
        if (!tags::writableExtension(t.path)) {
            ++out->skipped;
            continue;
        }
        const auto r = tags::read(t.path);
        if (!r.ok) {
            ST_LOG_INFO("tags", "can't read the tags of a file: {}", r.error);
            ++out->skipped;
            continue;
        }
        const auto& g = r.tags;
        out->title.merge(g.title);
        out->artist.merge(g.artist);
        out->album.merge(g.album);
        out->albumArtist.merge(g.albumArtist);
        out->genre.merge(g.genre);
        out->track.merge(number(g.track));
        out->disc.merge(number(g.disc));
        out->year.merge(number(g.year));
        if (firstCover) {
            out->cover = g.cover;
            firstCover = false;
        } else if (g.cover.has_value() != out->cover.has_value() || (g.cover && g.cover->data != out->cover->data)) {
            out->coverMixed = true;
        }
        const std::string f = tags::formatName(r.format);
        if (std::find(out->formats.begin(), out->formats.end(), f) == out->formats.end()) out->formats.push_back(f);
        out->targets.push_back(std::move(t));
    }
    return out;
}

const wchar_t* reason(tags::Status s) {
    switch (s) {
    case tags::Status::ReadOnly: return tr(L"Dosya salt okunur ya da klasöre yazılamıyor.");
    case tags::Status::InUse: return tr(L"Dosya başka bir uygulamada açık.");
    case tags::Status::Corrupt: return tr(L"Dosyanın etiketleri bozuk.");
    case tags::Status::Unsupported: return tr(L"Bu dosya biçiminin etiketleri düzenlenemiyor.");
    case tags::Status::NotFound: return tr(L"Dosya bulunamadı.");
    case tags::Status::VerifyFailed: return tr(L"Yeni dosya doğrulanamadı.");
    case tags::Status::Changed: return tr(L"Dosya bu sırada başka bir programca değiştirildi.");
    default: return tr(L"Dosyaya yazılamadı.");
    }
}

struct Outcome {
    Target target;
    tags::Status status = tags::Status::IoError;
    std::optional<local::Entry> fresh;   // the file re-read (library / dropped files)
};

// UI thread: the written files show their new tags everywhere, without a rescan.
void applyResults(const std::vector<Outcome>& results) {
    std::vector<local::Entry> library;
    std::vector<local::Entry> droppedFiles;
    int ok = 0, failed = 0;
    tags::Status firstError = tags::Status::Ok;
    for (const auto& r : results) {
        if (r.status != tags::Status::Ok) {
            ++failed;
            if (firstError == tags::Status::Ok) firstError = r.status;
            continue;
        }
        ++ok;
        if (!r.fresh) continue;
        // A download keeps its catalog metadata (its Spotify / MusicBrainz name); only a local copy of the path changes.
        const std::string id = local::trackIdFor(r.fresh->path);
        const bool inLibrary = LocalLibrary::get().entry(id) != nullptr;
        const bool isDropped = !inLibrary && r.target.track.id == id && !dropped::Store::get().pathFor(id).empty();
        if (inLibrary) library.push_back(*r.fresh);
        if (isDropped) droppedFiles.push_back(*r.fresh);
        if (!inLibrary && !isDropped) continue;
        const Track t = local::toTrack(*r.fresh);
        if (ctx().player) ctx().player->updateTrackInfo(t);
        ctx().library.updateTrackInfo(t);
    }
    LocalLibrary::get().updateEntries(std::move(library));
    if (!droppedFiles.empty()) dropped::Store::get().remember(droppedFiles);
    if (failed == 0) {
        toast(ok > 1 ? i18n::plural(L"{} dosyanın etiketleri kaydedildi", ok) : std::wstring(tr(L"Etiketler kaydedildi")));
        return;
    }
    const std::wstring why = reason(firstError);
    if (ok == 0 && failed == 1) {
        toast(i18n::format(tr(L"Etiketler kaydedilemedi: {} Dosyaya dokunulmadı."), {why}), true);
        return;
    }
    toast(i18n::format(tr(L"Kaydedilemeyen: {} / {}. {} O dosyalara dokunulmadı."),
                       {i18n::number(failed), i18n::number(failed + ok), why}),
          true);
}

void write(std::vector<Target> targets, tags::Edit edit) {
    g_busy = true;
    if (targets.size() > 1) toast(i18n::plural(L"{} dosyanın etiketleri kaydediliyor…", static_cast<int64_t>(targets.size())));
    const fs::path coverDir = LocalLibrary::coverFolder();
    async(
        Priority::Normal, g_life.ref(),
        [targets = std::move(targets), edit = std::move(edit), coverDir] {
            ComScope com;   // the property system re-reads the files
            std::vector<Outcome> out;
            for (const auto& t : targets) {
                Outcome o;
                o.target = t;
                o.status = tags::write(t.path, edit).status;
                if (o.status == tags::Status::Ok) {   // (a download may lie in a Yerel dosyalar folder too)
                    local::Entry e;
                    e.path = plainPath(t.path);
                    if (local::refreshEntry(e, coverDir)) o.fresh = std::move(e);
                }
                out.push_back(std::move(o));
            }
            return out;
        },
        [](Result<std::vector<Outcome>> r) {
            g_busy = false;
            if (!r) {
                toast(tr(L"Etiketler kaydedilemedi"), true);
                return;
            }
            applyResults(*r);
        });
}

// ---- The dialog ----------------------------------------------------------------------------------------------

// Cover tile (with "Görsel seç…" / "Kaldır" under it) + the fields.
class TagForm : public ui::Widget {
public:
    using Cover = tags::Edit::Cover;

    explicit TagForm(std::shared_ptr<const Loaded> loaded) : loaded_(std::move(loaded)) {
        const Loaded& l = *loaded_;
        const bool many = l.targets.size() > 1;
        addField(title_, tr(L"Başlık"), l.title);
        addField(artist_, tr(L"Sanatçı"), l.artist);
        addField(album_, tr(L"Albüm"), l.album);
        addField(albumArtist_, tr(L"Albüm sanatçısı"), l.albumArtist);
        addField(track_, tr(L"Parça no"), l.track);
        addField(disc_, tr(L"Disk no"), l.disc);
        addField(year_, tr(L"Yıl"), l.year);
        addField(genre_, tr(L"Tür"), l.genre);
        choose_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Görsel seç…"));
        choose_->onClick = [this] { pick(); };
        remove_ = add<ui::Button>(ui::ButtonKind::Ghost, tr(L"Kaldır"));
        remove_->setTooltip(tr(L"Kapağı dosyadan kaldır"));
        remove_->onClick = [this] {
            cover_ = Cover::Remove;
            jpeg_.clear();
            setPreview({});
            refresh();
        };
        if (l.cover) setPreview(pledit::saveCover(previewDir(), "tags", l.cover->data));
        std::wstring note;
        std::wstring formats;
        for (const auto& f : l.formats) formats += (formats.empty() ? L"" : L", ") + toWide(f);
        if (many)
            note = i18n::format(tr(L"{} · {}. \"Farklı değerler\" yazan alanlar, bir şey yazmadıkça olduğu gibi kalır."),
                                {i18n::plural(L"{} dosya", static_cast<int64_t>(l.targets.size())), formats});
        else
            note = fs::path(l.targets.front().path).filename().wstring() + L" · " + formats;
        if (l.skipped > 0) note += L" " + i18n::plural(L"{} dosyanın biçimi düzenlenemediği için atlanıyor.", l.skipped);
        note_ = add<ui::Label>(note, type::caption, ui::Tone::Tertiary);
        note_->setWrap(true, 3);
        refresh();
    }
    ~TagForm() override {
        if (!previewFile_.empty()) pledit::deleteCover(previewDir(), previewFile_);
    }

    void setSaveButton(ui::Button* b) {
        save_ = b;
        refresh();
    }
    ui::TextBox* firstBox() const { return title_.box; }
    std::function<void()> onSubmit;   // Enter in a field

    // The edit, or nullopt (with the bad fields marked) when a number isn't one.
    std::optional<tags::Edit> edit() {
        tags::Edit e;
        bool bad = false;
        text(title_, e.title);
        text(artist_, e.artist);
        text(album_, e.album);
        text(albumArtist_, e.albumArtist);
        text(genre_, e.genre);
        bad |= !numberField(track_, e.track, 9999);
        bad |= !numberField(disc_, e.disc, 999);
        bad |= !numberField(year_, e.year, 9999);
        if (bad) return std::nullopt;
        e.cover = cover_;
        if (cover_ == Cover::Set) {
            e.picture.data = jpeg_;
            e.picture.mime = "image/jpeg";
        }
        return e;
    }

    static constexpr float kSide = 160, kGap = 24, kRowH = 16 + 4 + 44, kRowGap = 12;
    float preferredHeight(float width) override {
        const float fields = 5 * kRowH + 4 * kRowGap;
        const float left = kSide + 12 + 36;
        return std::max(fields, left) + 16 + note_->preferredHeight(width);
    }

    void layout() override {
        const float w = rect().w;
        const float x = kSide + kGap, fw = std::max(160.f, w - x);
        const float half = (fw - 16) / 2, third = (fw - 32) / 3;
        float y = 0;
        place(title_, x, y, fw);
        y += kRowH + kRowGap;
        place(artist_, x, y, fw);
        y += kRowH + kRowGap;
        place(album_, x, y, half);
        place(albumArtist_, x + half + 16, y, half);
        y += kRowH + kRowGap;
        place(track_, x, y, third);
        place(disc_, x + third + 16, y, third);
        place(year_, x + 2 * (third + 16), y, third);
        y += kRowH + kRowGap;
        place(genre_, x, y, fw);
        y += kRowH;
        const float cw = choose_->naturalWidth();
        choose_->setRect({0, kSide + 12, cw, 36});
        remove_->setRect({cw + 4, kSide + 12, remove_->naturalWidth(), 36});
        const float noteY = std::max(y, kSide + 12 + 36) + 16;
        note_->setRect({0, noteY, w, note_->preferredHeight(w)});
    }

    void paint(Canvas& c) override {
        const Rect tile = coverRect();
        c.shadow(tile, 2, 24, 10, colors().shadowCard.mulAlpha(0.5f));
        const bool showMixed = loaded_->coverMixed && cover_ == Cover::Keep;
        if (showMixed) {   // the files' covers differ: a plain surface that says so
            c.fillRounded(tile, 2, gfx::isLightTheme() ? colors().bgSunken : colors().bgOverlay);
            c.icon("album", {tile.cx() - 16, tile.cy() - 30, 32, 32}, colors().fgTertiary);
            c.text(tr(L"Farklı kapaklar"), type::caption, {tile.x + 8, tile.cy() + 10, tile.w - 16, 18}, colors().fgSecondary,
                   gfx::TextAlign::Center);
        } else if (cover_ == Cover::Remove || previewUrl_.empty()) {
            drawArtwork(c, {}, tile, 2, Placeholder::Album);
        } else {
            drawArtwork(c, {{previewUrl_, 0, 0}}, tile, 2, Placeholder::Album);
        }
        if (busy_) {
            c.skeleton(tile, 2, ui::frame::now());
            ui::frame::requestNext();
        } else if (hover_) {
            c.fillRounded(tile, 2, Color{0, 0, 0, 0.55f});
            c.icon("edit", tile.center(32, 32), Color{1, 1, 1, 1});
        }
        paintChildren(c);
    }

    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left && coverRect().contains(e.pos); }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (coverRect().contains(e.pos)) pick();
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        const bool h = coverRect().contains(e.pos);
        if (h != hover_) {
            hover_ = h;
            invalidate();
        }
    }
    void onMouseLeave() override {
        hover_ = false;
        invalidate();
    }
    LPCWSTR cursor() const override { return hover_ && !busy_ ? IDC_HAND : IDC_ARROW; }

private:
    struct Box {
        ui::Label* caption = nullptr;
        ui::TextBox* box = nullptr;
        std::string original;
        bool mixed = false;
    };

    void addField(Box& f, const wchar_t* caption, const Field& value) {
        f.caption = add<ui::Label>(caption, type::caption, ui::Tone::Secondary);
        f.box = add<ui::TextBox>(ui::TextBox::Look::Field, value.mixed ? std::wstring(tr(L"Farklı değerler")) : std::wstring());
        f.mixed = value.mixed;
        f.original = value.mixed ? std::string() : value.value;
        f.box->setText(toWide(f.original));
        f.box->onSubmit = [this](const std::wstring&) {
            if (onSubmit) {
                const auto submit = onSubmit;
                submit();
            }
        };
        f.box->onChange = [b = f.box](const std::wstring&) { b->setError(false); };
    }
    static void place(Box& f, float x, float y, float w) {
        f.caption->setRect({x, y, w, 16});
        f.box->setRect({x, y + 20, w, 44});
    }

    static std::string trimmed(const std::wstring& s) {
        const auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
        size_t a = 0, b = s.size();
        while (a < b && ws(s[a])) ++a;
        while (b > a && ws(s[b - 1])) --b;
        return toUtf8(s.substr(a, b - a));
    }
    // A mixed field changes only when something is typed in; any other field when its text changed.
    static void text(const Box& f, std::optional<std::string>& out) {
        const std::string v = trimmed(f.box->text());
        if (f.mixed ? !v.empty() : v != f.original) out = v;
    }
    static bool numberField(const Box& f, std::optional<int>& out, int max) {
        const std::string v = trimmed(f.box->text());
        if (f.mixed ? v.empty() : v == f.original) return true;
        if (v.empty()) {
            out = 0;
            return true;
        }
        if (v.size() > 4 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; }) || std::stoi(v) > max) {
            f.box->setError(true);
            return false;
        }
        out = std::stoi(v);
        return true;
    }

    Rect coverRect() const { return {rect().x, rect().y, kSide, kSide}; }

    void refresh() {
        const bool any = cover_ == Cover::Set || (cover_ == Cover::Keep && (loaded_->cover.has_value() || loaded_->coverMixed));
        remove_->setVisible(any && !busy_);
        choose_->setEnabled(!busy_);
        if (save_) save_->setEnabled(!busy_);
        invalidate();
    }

    void pick() {
        if (busy_) return;
        Dispatcher::post([ref = life_.ref(), this] {
            if (ref.expired()) return;
            const std::wstring file = pickImageFile();
            if (file.empty() || ref.expired()) return;
            busy_ = true;
            refresh();
            // The library's cover scaling: a centered square JPEG (a bit larger than a playlist's).
            async(
                Priority::High, life_.ref(), [file] { return pledit::squareJpeg(fs::path(file), 1000, 600 * 1024); },
                [this](Result<pledit::Cover> r) {
                    busy_ = false;
                    if (!r || r->jpeg.empty()) {
                        toast(tr(L"Görsel okunamadı"), true);
                        refresh();
                        return;
                    }
                    jpeg_ = std::move(r->jpeg);
                    cover_ = Cover::Set;
                    setPreview(pledit::saveCover(previewDir(), "tags", jpeg_));
                    refresh();
                });
        });
    }

    void setPreview(std::wstring file) {
        if (!previewFile_.empty()) pledit::deleteCover(previewDir(), previewFile_);
        previewFile_ = std::move(file);
        previewUrl_ = previewFile_.empty() ? std::string() : local::fileUrl(previewFile_);
        invalidate();
    }

    std::shared_ptr<const Loaded> loaded_;
    Box title_, artist_, album_, albumArtist_, track_, disc_, year_, genre_;
    ui::Button *choose_ = nullptr, *remove_ = nullptr, *save_ = nullptr;
    ui::Label* note_ = nullptr;
    Cover cover_ = Cover::Keep;
    std::vector<uint8_t> jpeg_;
    std::wstring previewFile_;
    std::string previewUrl_;
    bool busy_ = false, hover_ = false;
    Lifetime life_;
};

void showDialog(std::shared_ptr<Loaded> loaded) {
    if (loaded->targets.empty()) {
        toast(tr(L"Bu dosyaların etiketleri okunamadı"), true);
        return;
    }
    auto* d = ui::Dialog::open(ctx().window, tr(L"Etiketleri düzenle"), {}, 680);
    if (!d) return;
    auto* form = d->body()->add<TagForm>(loaded);
    auto save = [d, form, loaded] {
        const auto e = form->edit();
        if (!e) {
            toast(tr(L"Parça no, disk no ve yıl yalnızca rakamlardan oluşabilir"), true);
            return;
        }
        if (!e->empty()) write(loaded->targets, *e);
        d->close();
    };
    form->onSubmit = save;
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    form->setSaveButton(d->addButton(tr(L"Kaydet"), ui::ButtonKind::Primary, save, false));
    form->firstBox()->focus();
    form->firstBox()->selectAll();
}

} // namespace

std::wstring fileFor(const Track& t) {
    if (t.id.rfind("local:", 0) == 0) {
        if (const auto* e = LocalLibrary::get().entry(t.id)) return e->path;
        return dropped::Store::get().pathFor(t.id);
    }
    if (const auto* d = ctx().downloads.item(t.id); d && d->state == DlState::Done && !d->filePath.empty()) return toWide(d->filePath);
    return {};
}

std::optional<ui::MenuItem> menuItem(const std::vector<Track>& tracks) {
    size_t files = 0, writable = 0;
    for (const auto& t : tracks) {
        const std::wstring f = fileFor(t);
        if (f.empty()) continue;
        ++files;
        if (tags::writableExtension(f)) ++writable;
    }
    if (files == 0) return std::nullopt;
    ui::MenuItem it{tr(L"Etiketleri düzenle…"), "edit", L"", [tracks] { open(tracks); }};
    if (writable == 0) {
        it.enabled = false;
        it.tooltip = files > 1 ? tr(L"Bu biçimlerin etiketleri düzenlenemiyor (MP3, M4A ve FLAC düzenlenebilir)")
                               : tr(L"Bu biçimin etiketleri düzenlenemiyor (MP3, M4A ve FLAC düzenlenebilir)");
    }
    return it;
}

void open(const std::vector<Track>& tracks) {
    if (g_busy) {
        toast(tr(L"Etiketler hâlâ kaydediliyor; biraz sonra yeniden dene"));
        return;
    }
    auto targets = targetsFor(tracks);
    if (targets.empty()) return;
    if (targets.size() > kMaxFiles) {
        toast(i18n::plural(L"En fazla {} dosyanın etiketleri birlikte düzenlenebilir", static_cast<int64_t>(kMaxFiles)), true);
        return;
    }
    g_busy = true;
    async(
        Priority::High, g_life.ref(), [targets = std::move(targets)]() mutable { return load(std::move(targets)); },
        [](Result<std::shared_ptr<Loaded>> r) {
            g_busy = false;
            if (!r) {
                toast(tr(L"Bu dosyaların etiketleri okunamadı"), true);
                return;
            }
            showDialog(*r);
        });
}

void initDevHook() {
    wchar_t buf[32] = {};
    if (GetEnvironmentVariableW(L"SHADETUBE_TAG_EDITOR", buf, 32) == 0) return;
    // "<n>" = the first n songs, "<first>,<n>" = n songs from index first.
    const wchar_t* comma = wcschr(buf, L',');
    const size_t first = comma ? static_cast<size_t>(std::max(0, _wtoi(buf))) : 0;
    const size_t n = static_cast<size_t>(std::max(1, _wtoi(comma ? comma + 1 : buf)));
    auto done = std::make_shared<bool>(false);
    ctx().housekeepingHooks.push_back([first, n, done] {
        const auto& tracks = LocalLibrary::get().tracks();
        if (*done || !LocalLibrary::get().loaded() || tracks.size() <= first) return;
        *done = true;
        const auto from = tracks.begin() + static_cast<std::ptrdiff_t>(first);
        open(std::vector<Track>(from, from + static_cast<std::ptrdiff_t>(std::min(n, tracks.size() - first))));
    });
}

} // namespace st::app::tagedit
