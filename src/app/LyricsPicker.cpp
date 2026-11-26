#include "app/LyricsPicker.h"

#include "app/AppContext.h"
#include "app/Components.h"
#include "app/PageWidgets.h"
#include "app/InternetRadio.h"
#include "app/LyricsService.h"
#include "app/Source.h"
#include "catalog/TrackKind.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "lyrics/Lyrics.h"
#include "musicbrainz/Parse.h"
#include "ui/Layout.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <memory>

namespace st::app {

using gfx::accent;
using gfx::colors;
using Microsoft::WRL::ComPtr;
using Kind = lyrics::Choice::Kind;
namespace type = gfx::type;

namespace {

constexpr float kListH = 336;

// "+12 sn fark" / "−3 sn fark" / "Süre aynı" against the song's length; "" when either is unknown.
std::wstring durationHint(int hitSec, int songSec) {
    if (hitSec <= 0 || songSec <= 0) return {};
    const int d = hitSec - songSec;
    if (std::abs(d) <= 1) return tr(L"Süre aynı");
    return i18n::format(tr(L"{} sn fark"), {(d > 0 ? L"+" : L"−") + std::to_wstring(std::abs(d))});
}

// .lrc / .txt picker (IFileOpenDialog) owned by the main window. Modal: call it posted (outside a widget event).
std::wstring pickLyricsFile() {
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return {};
    FILEOPENDIALOGOPTIONS opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    const std::wstring files = tr(L"Şarkı sözleri (.lrc, .txt)"), all = tr(L"Tüm dosyalar");
    const COMDLG_FILTERSPEC types[] = {{files.c_str(), L"*.lrc;*.txt"}, {all.c_str(), L"*.*"}};
    dlg->SetFileTypes(2, types);
    const std::wstring title = tr(L"Söz dosyası seç");
    dlg->SetTitle(title.c_str());
    if (dlg->Show(ctx().window ? ctx().window->hwnd() : nullptr) != S_OK) return {};
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (FAILED(dlg->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    std::wstring file = path;
    CoTaskMemFree(path);
    return file;
}

// The file picker, then the copy into the profile (worker), then the choice.
void loadLyricsFile(const std::string& trackId) {
    Dispatcher::post([trackId] {
        const std::wstring path = pickLyricsFile();
        if (path.empty()) return;
        auto keep = std::make_shared<Lifetime>();
        async(Priority::High, keep->ref(), [trackId, path] { return lyrics::Offsets::get().importFile(trackId, path); },
              [keep, trackId](Result<std::string> r) {
                  if (!r || r->empty()) {
                      toast(tr(L"Bu dosyada kullanılabilir söz bulunamadı."), true);
                      return;
                  }
                  lyrics::Choice c;
                  c.kind = Kind::File;
                  c.file = *r;
                  setLyricsChoice(trackId, c);
                  toast(tr(L"Sözler dosyadan yüklendi"));
              });
    });
}

// One LRCLIB record: "title — artist", album · length · the difference to the song, the kind, a preview of the text.
class HitRow : public ui::Widget {
public:
    HitRow(const lyrics::SearchHit& h, int songSec, bool current) : hit_(h), current_(current) {
        focusable = true;
        title_ = gfx::Text(toWide(h.title) + (h.artist.empty() ? L"" : L" — " + toWide(h.artist)), type::body);
        std::wstring meta = toWide(h.album);
        if (h.durationSec > 0) meta += (meta.empty() ? L"" : L" · ") + ui::formatDuration(int64_t{h.durationSec} * 1000);
        if (const std::wstring hint = durationHint(h.durationSec, songSec); !hint.empty()) meta += L" · " + hint;
        meta_ = gfx::Text(meta, type::caption);
        far_ = songSec > 0 && h.durationSec > 0 && std::abs(h.durationSec - songSec) > 5;
        const std::wstring kindLabel = h.instrumental ? tr(L"Enstrümantal") : h.synced ? tr(L"Senkronize") : tr(L"Düz metin");
        gfx::TextOptions centered;
        centered.align = gfx::TextAlign::Center;
        badge_ = gfx::Text(toUpperTr(current ? std::wstring(tr(L"Şu an")) + L" · " + kindLabel : kindLabel), type::monoBadge,
                           centered);
        preview_ = gfx::Text(h.instrumental ? std::wstring() : toWide(lyrics::previewText(h.lyrics)), type::caption);
        setTooltip(toWide(lyrics::previewText(h.lyrics, 4)));
    }
    std::function<void()> onPick;

    bool activatable() const override { return true; }
    bool onActivate() override {
        if (!onPick) return false;
        const auto pick = onPick;   // closes the dialog
        pick();
        return true;
    }
    float preferredHeight(float) override { return preview_.empty() ? 56.f : 74.f; }
    bool onMouseDown(const ui::MouseEvent& e) override { return e.button == ui::MouseButton::Left; }
    void onMouseUp(const ui::MouseEvent& e) override {
        if (rect().contains(e.pos) && onPick) {
            const auto pick = onPick;
            pick();
        }
    }
    void onMouseEnter() override { hover_.to(1, ui::motion::fast); }
    void onMouseLeave() override { hover_.to(0, ui::motion::fast); }
    LPCWSTR cursor() const override { return IDC_HAND; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        if (current_) {
            c.fillRect(r, accent().tint12);
            c.fillRect({r.x, r.y, 2, r.h}, accent().base);
        } else {
            c.fillRect(r, col.overlayHover.mulAlpha(hover_));
        }
        const float bw = std::ceil(badge_.measure().w) + 14;
        const float x = r.x + 12, w = std::max(40.f, r.w - 24 - bw - 12);
        c.text(title_, {x, r.y + 9, w, 20}, current_ ? accent().base : col.fgPrimary, gfx::VAlign::Center);
        // The kind as a small outlined tag on the right (the accent for synced lyrics).
        const Rect tag{r.right() - 12 - bw, r.y + 10, bw, 18};
        const Color tagColor = hit_.synced ? accent().base : col.fgTertiary;
        c.strokeRounded(tag, gfx::radii().badge, tagColor.withAlpha(0.6f));
        c.text(badge_, tag, tagColor, gfx::VAlign::Center);
        c.text(meta_, {x, r.y + 30, r.w - 24, 16}, far_ ? col.warning : col.fgSecondary, gfx::VAlign::Center);
        if (!preview_.empty()) c.text(preview_, {x, r.y + 49, r.w - 24, 16}, col.fgTertiary, gfx::VAlign::Center);
    }

private:
    lyrics::SearchHit hit_;
    bool current_;
    bool far_ = false;
    gfx::Text title_, meta_, badge_, preview_;
    ui::Anim hover_;
};

// The dialog's content: the query field, the results (or what happened), the other choices.
class PickerBody : public ui::Widget {
public:
    PickerBody(const catalog::Track& t, int64_t shownId, ui::Dialog* dialog) : track_(t), shownId_(shownId), dialog_(dialog) {
        choice_ = lyricsChoice(t.id);
        query_ = add<ui::TextBox>(ui::TextBox::Look::Field, tr(L"Şarkı ya da sanatçı ara"));
        std::string q = mb::detail::searchableTitle(t.name);
        if (!t.artists.empty() && !t.artists[0].name.empty()) q = t.artists[0].name + " " + q;
        query_->setText(toWide(q));
        query_->onSubmit = [this](const std::wstring& text) { search(text); };
        scroll_ = add<ui::ScrollView>();
        list_ = scroll_->setContent<ui::Column>(2.f);
        status_ = add<ui::Label>(std::wstring(), type::secondary, ui::Tone::Tertiary);
        status_->setWrap(true);
        // The other choices.
        const bool spotify = t.id.rfind("spotify:track:", 0) == 0 && source::activeApi();
        if (spotify) {
            spotify_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Spotify'ın sözleri"));
            spotify_->setTooltip(tr(L"Spotify'ın bu şarkı için verdiği sözleri kullan"));
            spotify_->setActive(choice_.kind == Kind::Spotify);
            spotify_->onClick = [this] { pick(lyrics::Choice{Kind::Spotify}, tr(L"Sözler: Spotify")); };
        }
        file_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Dosyadan yükle…"));
        file_->setTooltip(tr(L"Bilgisayarındaki bir .lrc ya da .txt dosyasını kullan"));
        file_->setActive(choice_.kind == Kind::File);
        file_->onClick = [this] {
            const std::string id = track_.id;
            dialog_->close();
            loadLyricsFile(id);
        };
        none_ = add<ui::Button>(ui::ButtonKind::Secondary, tr(L"Söz yok"));
        none_->setTooltip(tr(L"Bu şarkıda söz gösterme"));
        none_->setActive(choice_.kind == Kind::None);
        none_->onClick = [this] { pick(lyrics::Choice{Kind::None}, tr(L"Bu şarkıda söz gösterilmeyecek")); };
        search(query_->text());
    }
    ~PickerBody() override { life_.renew(); }

    ui::TextBox* field() const { return query_; }
    float preferredHeight(float) override { return 40 + 12 + kListH + 16 + 36; }
    void layout() override {
        const Rect r = rect();
        query_->setRect({0, 0, r.w, 40});
        const Rect list{0, 52, r.w, kListH};
        scroll_->setRect(list);
        status_->setRect({4, list.y + 8, r.w - 8, 40});
        float x = 0;
        const float y = list.bottom() + 16;
        for (auto* b : {spotify_, file_, none_}) {
            if (!b) continue;
            const float w = std::ceil(b->naturalWidth());
            b->setRect({x, y, w, 36});
            x += w + 8;
        }
    }
    void paint(Canvas& c) override {
        // The list's frame, like the other pickers' lists.
        c.strokeRounded(scroll_->rect(), gfx::radii().field, colors().hairDefault);
        paintChildren(c);
    }

private:
    void search(const std::wstring& text) {
        life_.renew();
        list_->clearChildren();
        // The shared states (app/PageWidgets): result-row placeholders while LRCLIB answers, then the results, or the
        // compact error / no-result state in the list's frame.
        list_->add<SkeletonLines>(5, 52.f);
        scroll_->contentChanged();
        status_->setVisible(false);
        requestLayout();
        invalidate();
        const int songSec = track_.durationMs / 1000;
        async(Priority::High, life_.ref(),
              [q = toUtf8(text), songSec] {
                  auto hits = lyrics::search(q);
                  if (hits) lyrics::rankHits(*hits, songSec);
                  return hits;
              },
              [this, songSec](Result<std::optional<std::vector<lyrics::SearchHit>>> r) {
                  list_->clearChildren();
                  if (!r || !*r || (*r)->empty()) {
                      if (!r || !*r) {
                          auto* p = list_->add<MessagePanel>("offline", tr(L"LRCLIB'e ulaşılamadı. Bağlantını kontrol et."),
                                                             std::wstring(), tr(L"Tekrar dene"), [this, ref = life_.ref()] {
                                                                 Dispatcher::post([this, ref] {   // after the click
                                                                     if (!ref.expired()) search(query_->text());
                                                                 });
                                                             });
                          p->setTone(MessagePanel::Tone::Offline).setCompact(true);
                      } else {
                          noResultsState(list_, tr(L"Sonuç bulunamadı. Aramayı değiştirip tekrar dene."));
                      }
                      scroll_->contentChanged();
                      requestLayout();
                      invalidate();
                      return;
                  }
                  const int64_t current = choice_.kind == Kind::Lrclib ? choice_.lrclibId : choice_.kind == Kind::Auto ? shownId_ : 0;
                  for (const auto& h : **r) {
                      auto* row = list_->add<HitRow>(h, songSec, h.id == current);
                      row->onPick = [this, h] { pickRecord(h); };
                  }
                  scroll_->contentChanged();
                  requestLayout();
                  invalidate();
              });
    }

    void pick(const lyrics::Choice& c, const std::wstring& note) {
        const std::string id = track_.id;
        dialog_->close();
        setLyricsChoice(id, c);
        toast(note);
    }

    // The record's lyrics go into the cache first (no second request), then the choice.
    void pickRecord(const lyrics::SearchHit& h) {
        const std::string id = track_.id;
        dialog_->close();
        auto keep = std::make_shared<Lifetime>();
        async(Priority::High, keep->ref(),
              [h] {
                  lyrics::rememberRecord(h);
                  return Unit{};
              },
              [keep, id, recordId = h.id](Result<Unit>) {
                  lyrics::Choice c;
                  c.kind = Kind::Lrclib;
                  c.lrclibId = recordId;
                  setLyricsChoice(id, c);
                  toast(tr(L"Sözler güncellendi"));
              });
    }

    catalog::Track track_;
    int64_t shownId_;
    ui::Dialog* dialog_;
    lyrics::Choice choice_;
    ui::TextBox* query_;
    ui::ScrollView* scroll_;
    ui::Column* list_;
    ui::Label* status_;
    ui::Button* spotify_ = nullptr;
    ui::Button* file_ = nullptr;
    ui::Button* none_ = nullptr;
    Lifetime life_;
};

} // namespace

void showLyricsPicker(const catalog::Track& t, int64_t shownLrclibId) {
    if (radio::isStationId(t.id) || catalog::isPodcastId(t.id) || t.id.empty() || !ctx().window) return;
    auto* d = ui::Dialog::open(ctx().window, tr(L"Yanlış sözler mi?"),
                               i18n::format(tr(L"\"{}\" için LRCLIB'de bulunan sözler. Seçtiğin sözler bu şarkı için "
                                               L"hatırlanır; zamanlama ayarı sıfırlanır."),
                                            {toWide(t.name)}),
                               640);
    if (!d) return;
    auto* body = d->body()->add<PickerBody>(t, shownLrclibId, d);
    if (lyricsChoice(t.id).kind != Kind::Auto) {
        const std::string id = t.id;
        d->addButton(tr(L"Otomatik seçime dön"), ui::ButtonKind::Ghost, [id] {
            setLyricsChoice(id, lyrics::Choice{});
            toast(tr(L"Sözler yeniden otomatik seçiliyor"));
        });
    }
    d->addButton(tr(L"Kapat"), ui::ButtonKind::Ghost, {});
    body->field()->focus();
}

} // namespace st::app
