// "Ayrıntıları düzenle": the name, description and cover of a playlist (Spotify's "Edit details" dialog). Opened from a
// playlist's cover / title / "⋯" menu and the sidebar. A picked picture (JPEG / PNG / WebP...) is cropped to a centered
// square and encoded as a JPEG of at most 256 KB on a worker (app/PlaylistEditing); Spotify gets it uploaded, a local
// playlist keeps it in %LOCALAPPDATA%\ShadeTube\playlist-covers.
#include "app/AppContext.h"
#include "app/Components.h"
#include "app/LocalLibrary.h"
#include "app/PlaylistEditing.h"
#include "app/Source.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Log.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "spotify/Session.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <filesystem>

namespace st::app {

using gfx::accent;
using gfx::colors;
using Microsoft::WRL::ComPtr;
namespace type = gfx::type;

namespace {

constexpr size_t kMaxDescription = 300;   // Spotify's limit (characters)

std::wstring trimmed(std::wstring s) {
    const auto ws = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!s.empty() && ws(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && ws(s[i])) ++i;
    return s.substr(i);
}

// Picture previews of the open dialog (deleted when it closes).
std::filesystem::path previewDir() {
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / L"ShadeTube";
}

// Image file picker (IFileOpenDialog) owned by the main window. Modal: call it posted (outside a widget event).
std::wstring pickImageFile() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const std::wstring images = tr(L"Görseller (JPEG, PNG, WebP)"), all = tr(L"Tüm dosyalar");
    const COMDLG_FILTERSPEC types[] = {{images.c_str(), L"*.jpg;*.jpeg;*.jfif;*.png;*.webp;*.bmp;*.gif"},
                                       {all.c_str(), L"*.*"}};
    dlg->SetFileTypes(2, types);
    dlg->SetTitle(tr(L"Kapak görseli seç"));
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    std::wstring file = path;
    CoTaskMemFree(path);
    return file;
}

// Cover tile (click: pick a picture) + name and description fields + "Görsel seç…" / "Görseli kaldır".
class DetailsEditor : public ui::Widget {
public:
    using Cover = spotify::PlaylistDetailsChange::Cover;

    DetailsEditor(const std::wstring& name, const std::wstring& description, std::vector<catalog::Image> images,
                  bool hasCover)
        : images_(std::move(images)), hasCover_(hasCover) {
        nameLabel_ = add<ui::Label>(tr(L"Ad"), type::caption, ui::Tone::Secondary);
        name_ = add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Çalma listesi adı"));
        name_->setText(name);
        descLabel_ = add<ui::Label>(tr(L"Açıklama"), type::caption, ui::Tone::Secondary);
        desc_ = add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Açıklama ekle (isteğe bağlı)"));
        desc_->setText(description);
        choose_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Görsel seç…"));
        choose_->onClick = [this] { pick(); };
        remove_ = add<ui::Button>(ui::ButtonKind::Ghost, tr(L"Görseli kaldır"));
        remove_->onClick = [this] {
            setPreview({});
            cover_ = Cover::Remove;
            jpeg_.clear();
            refreshButtons();
        };
        refreshButtons();
    }
    ~DetailsEditor() override {
        if (!previewFile_.empty()) pledit::deleteCover(previewDir(), previewFile_);
    }

    // The dialog's "Kaydet": disabled while a picture is being read.
    void setSaveButton(ui::Button* b) {
        save_ = b;
        refreshButtons();
    }
    ui::TextBox* nameBox() const { return name_; }
    std::wstring name() const { return trimmed(name_->text()); }
    std::wstring description() const {
        std::wstring d = trimmed(desc_->text());
        if (d.size() > kMaxDescription) d.resize(kMaxDescription);
        return d;
    }
    Cover cover() const { return cover_; }
    const std::vector<uint8_t>& jpeg() const { return jpeg_; }

    static constexpr float kSide = 192;
    float preferredHeight(float) override { return kSide; }

    void layout() override {
        const float w = rect().w;
        const float x = kSide + 24, fw = std::max(120.f, w - x);
        nameLabel_->setRect({x, 0, fw, 16});
        name_->setRect({x, 20, fw, 44});
        descLabel_->setRect({x, 76, fw, 16});
        desc_->setRect({x, 96, fw, 44});
        const float cw = choose_->naturalWidth();
        choose_->setRect({x, kSide - 36, cw, 36});
        remove_->setRect({x + cw + 8, kSide - 36, remove_->naturalWidth(), 36});
    }

    void paint(Canvas& c) override {
        const Rect cover = coverRect();
        c.shadow(cover, 2, 24, 10, colors().shadowCard.mulAlpha(0.5f));
        if (cover_ == Cover::Set) drawArtwork(c, {{previewUrl_, 0, 0}}, cover, 2, Placeholder::Playlist);
        else if (cover_ == Cover::Remove) drawArtwork(c, {}, cover, 2, Placeholder::Playlist);
        else drawArtwork(c, images_, cover, 2, Placeholder::Playlist);
        if (busy_) {
            c.skeleton(cover, 2, ui::frame::now());
            ui::frame::requestNext();
        } else if (hover_) {
            c.fillRounded(cover, 2, Color{0, 0, 0, 0.55f});
            c.icon("edit", cover.center(40, 40), Color{1, 1, 1, 1});
        }
        paintChildren(c);
    }

    // Pick on release (the file dialog is modal: posted, outside the mouse event).
    bool onMouseDown(const ui::MouseEvent& e) override {
        return e.button == ui::MouseButton::Left && coverRect().contains(e.pos);
    }
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
    Rect coverRect() const { return {rect().x, rect().y, kSide, kSide}; }

    void refreshButtons() {
        const bool any = cover_ == Cover::Set || (cover_ == Cover::Keep && hasCover_);
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
            refreshButtons();
            async(
                Priority::High, life_.ref(), [file] { return pledit::squareJpeg(std::filesystem::path(file)); },
                [this](Result<pledit::Cover> r) {
                    busy_ = false;
                    if (!r || r->jpeg.empty()) {
                        toast(tr(L"Görsel okunamadı"), true);
                        refreshButtons();
                        return;
                    }
                    ST_LOG_INFO("playlist", "cover picked: {} px, {} bytes", r->side, r->jpeg.size());
                    jpeg_ = std::move(r->jpeg);
                    cover_ = Cover::Set;
                    setPreview(pledit::saveCover(previewDir(), "preview", jpeg_));
                    refreshButtons();
                });
        });
    }

    // The picked picture, shown from a temporary file (gfx::ImageCache loads file:/// URLs).
    void setPreview(std::wstring file) {
        if (!previewFile_.empty()) pledit::deleteCover(previewDir(), previewFile_);
        previewFile_ = std::move(file);
        previewUrl_ = previewFile_.empty() ? std::string() : local::fileUrl(previewFile_);
        invalidate();
    }

    std::vector<catalog::Image> images_;
    bool hasCover_;
    ui::Label *nameLabel_, *descLabel_;
    ui::TextBox *name_, *desc_;
    ui::Button *choose_, *remove_, *save_ = nullptr;
    Cover cover_ = Cover::Keep;
    std::vector<uint8_t> jpeg_;
    std::wstring previewFile_;
    std::string previewUrl_;
    bool busy_ = false, hover_ = false;
    Lifetime life_;
};

// The picture is the user's own (not Spotify's mosaic of the first songs' covers).
bool customSpotifyCover(const std::vector<catalog::Image>& images) {
    return !images.empty() && images.front().url.find("mosaic") == std::string::npos;
}

} // namespace

void editPlaylistDetails(const std::string& id, const std::wstring& name, const std::wstring& description,
                         const std::vector<catalog::Image>& images) {
    const bool spotify = source::isSpotifyId(id);
    if (spotify && !(source::loggedIn() && ctx().session)) {
        toast(tr(L"Spotify bağlantısı yok"), true);
        return;
    }
    if (!spotify && !ctx().library.playlist(id)) return;
    auto* d = ui::Dialog::open(ctx().window, tr(L"Ayrıntıları düzenle"),
                               spotify ? tr(L"Değişiklikler Spotify hesabına kaydedilir.") : std::wstring(), 600);
    if (!d) return;
    const bool hasCover = spotify ? customSpotifyCover(images) : ctx().library.hasCustomCover(id);
    auto* editor = d->body()->add<DetailsEditor>(name, description, images, hasCover);
    auto apply = [editor, id, spotify, oldName = name, oldDesc = description] {
        std::wstring newName = editor->name();
        if (newName.empty()) newName = oldName;   // a playlist always has a name
        const std::wstring newDesc = editor->description();
        const auto cover = editor->cover();
        using Cover = spotify::PlaylistDetailsChange::Cover;
        if (spotify) {
            auto* s = source::loggedIn() ? ctx().session : nullptr;
            if (!s) {
                toast(tr(L"Spotify bağlantısı yok"), true);
                return;
            }
            spotify::PlaylistDetailsChange change;
            if (newName != oldName) change.name = toUtf8(newName);
            if (newDesc != oldDesc) change.description = toUtf8(newDesc);
            change.cover = cover;
            if (cover == Cover::Set) change.jpeg = editor->jpeg();
            if (change.empty()) return;
            s->updatePlaylistDetails(id, std::move(change), [](bool ok) {
                toast(ok ? tr(L"Ayrıntılar kaydedildi") : tr(L"Ayrıntılar kaydedilemedi"), !ok);
            });
            return;
        }
        auto& lib = ctx().library;
        if (!lib.playlist(id)) return;
        if (newName != oldName) lib.renamePlaylist(id, newName);
        if (newDesc != oldDesc) lib.setPlaylistDescription(id, newDesc);
        if (cover == Cover::Set && !lib.setPlaylistCover(id, &editor->jpeg())) toast(tr(L"Görsel kaydedilemedi"), true);
        else if (cover == Cover::Remove) lib.setPlaylistCover(id, nullptr);
    };
    d->addButton(tr(L"Vazgeç"), ui::ButtonKind::Ghost, {});
    editor->setSaveButton(d->addButton(tr(L"Kaydet"), ui::ButtonKind::Primary, apply));
    editor->nameBox()->focus();
    editor->nameBox()->selectAll();
}

} // namespace st::app
