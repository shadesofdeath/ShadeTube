// "Birlikte dinle": the side panel and the settings rows (the session itself is app/Together.cpp).
#include "app/Together.h"

#include "app/Commands.h"
#include "app/JamInvite.h"
#include "app/JamMqtt.h"
#include "app/PageWidgets.h"
#include "app/SettingsWidgets.h"
#include "core/Dispatcher.h"
#include "core/I18n.h"
#include "core/Settings.h"
#include "core/Utf.h"
#include "gfx/Theme.h"
#include "ui/Controls.h"
#include "ui/Layout.h"
#include "ui/Popups.h"
#include "ui/TextBox.h"
#include "ui/Window.h"

#include <algorithm>
#include <cmath>

namespace st::app {

using gfx::accent;
using gfx::colors;
using ui::Button;
using ui::ButtonKind;
namespace type = gfx::type;

namespace {

constexpr float kPadX = 20;
constexpr float kListTop = 20 + 36 + 12;

// Buttons at their natural width, left to right, wrapping onto a new line when the panel is too narrow.
class ButtonRow : public ui::Widget {
public:
    ButtonRow() { hitTestVisible = false; }
    Button* button(ButtonKind kind, std::wstring label, std::string icon, std::function<void()> fn) {
        auto* b = add<Button>(kind, std::move(label), std::move(icon));
        b->onClick = std::move(fn);
        return b;
    }
    float preferredHeight(float w) override { return place(w, false); }
    void layout() override { place(rect().w, true); }

private:
    float place(float w, bool apply) {
        float x = 0, y = 0;
        for (auto& child : children()) {
            auto* b = static_cast<Button*>(child.get());
            const float bw = std::min(w, std::ceil(b->naturalWidth()));
            if (x > 0 && x + bw > w) {
                x = 0;
                y += 40 + 8;
            }
            if (apply) b->setRect({x, y, bw, 40});
            x += bw + 8;
        }
        return children().empty() ? 0.f : y + 40;
    }
};

// "● Bağlı · broker.emqx.io" — read live from the session at every paint.
class StatusLine : public ui::Widget {
public:
    StatusLine() { hitTestVisible = false; }
    float preferredHeight(float) override { return 20; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto v = together::view();
        Color dot = col.fgTertiary;
        std::wstring text;
        switch (v.link) {
        case together::Link::Connected:
            dot = col.success;
            text = i18n::format(tr(L"Bağlı · {}"), {toWide(v.broker)});
            break;
        case together::Link::Connecting:
            dot = col.warning;
            text = v.broker.empty() ? std::wstring(tr(L"Sunucuya bağlanılıyor…"))
                                    : i18n::format(tr(L"Bağlanılıyor · {}"), {toWide(v.broker)});
            break;
        case together::Link::Reconnecting:
            dot = col.warning;
            text = i18n::format(tr(L"Yeniden bağlanılıyor · {}"), {toWide(v.broker)});
            break;
        case together::Link::Off: text = tr(L"Bağlı değil"); break;
        }
        c.fillCircle({r.x + 4, r.cy()}, 4, dot);
        c.text(text, type::caption, {r.x + 16, r.y, r.w - 16, r.h}, col.fgSecondary, gfx::TextAlign::Leading, gfx::VAlign::Center);
    }
};

// The guest's "Eşit · ±0,12 sn" / why it holds.
class SyncLine : public ui::Widget {
public:
    SyncLine() { hitTestVisible = false; }
    float preferredHeight(float w) override {
        gfx::Text t(together::syncText(), type::secondary, {gfx::TextAlign::Leading, true, true, 4});
        return std::max(20.f, std::ceil(t.measure(w - 16).h));
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const bool ok = together::syncOk();
        c.icon(ok ? "check" : "clock", {r.x, r.y + 3, 12, 12}, ok ? accent().base : col.fgTertiary);
        gfx::Text t(together::syncText(), type::secondary, {gfx::TextAlign::Leading, true, true, 4});
        c.text(t, {r.x + 18, r.y, r.w - 18, r.h + 40}, col.fgSecondary);
    }
};

// The invite link, mono, in a raised box (selectable by copying only: the button below copies it).
class InviteBox : public ui::Widget {
public:
    explicit InviteBox(std::string link) : text_(toWide(link), type::monoMeta) { hitTestVisible = false; }
    float preferredHeight(float) override { return 40; }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        c.fillRounded(r, 2, col.bgRaised);
        c.strokeRounded(r, 2, col.hairDefault);
        c.icon("link", {r.x + 12, r.cy() - 7, 14, 14}, col.fgTertiary);
        if (text_.empty()) {
            c.text(tr(L"Sunucuya bağlanınca hazır olur…"), type::caption, {r.x + 34, r.y, r.w - 46, r.h}, col.fgTertiary,
                   gfx::TextAlign::Leading, gfx::VAlign::Center);
            return;
        }
        c.text(text_, {r.x + 34, r.y, r.w - 46, r.h}, col.fgSecondary, gfx::VAlign::Center);
    }

private:
    gfx::Text text_;
};

// A participant: initial in a circle, name, what they are; the host may remove guests.
class MemberRow : public ui::Widget {
public:
    MemberRow(const together::MemberView& m, bool removable)
        : m_(m), name_(m.name, type::body.withSize(13)), status_(statusOf(m), type::caption) {
        if (removable) {
            remove_ = add<Button>(ButtonKind::Icon, L"", "close");
            remove_->setIconSize(14);
            remove_->setTooltip(i18n::format(tr(L"{} oturumdan çıkar"), {m.name}));
            remove_->onClick = [device = m.device, name = m.name] {
                ui::Dialog::confirm(ctx().window, tr(L"Oturumdan çıkar"),
                                    i18n::format(tr(L"{} birlikte dinleme oturumundan çıkarılsın mı? Bu cihaz bu oturuma bir "
                                                    L"daha katılamaz."),
                                                 {name}),
                                    tr(L"Çıkar"), [device] { together::removeMember(device); }, true);
            };
        }
    }
    float preferredHeight(float) override { return 52; }
    void layout() override {
        if (remove_) remove_->setRect({rect().w - 32, 10, 32, 32});
    }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const auto& acc = accent();
        const float alpha = m_.online ? 1.f : 0.5f;
        const Rect av{r.x, r.cy() - 16, 32, 32};
        c.fillCircle({av.cx(), av.cy()}, 16, m_.host ? acc.tint12 : col.bgOverlay);
        std::wstring initial = m_.name.empty() ? L"?" : toUpperTr(m_.name.substr(0, 1));
        c.text(initial, type::body.withWeight(700), av, (m_.host ? acc.base : col.fgSecondary).mulAlpha(alpha), gfx::TextAlign::Center,
               gfx::VAlign::Center);
        if (m_.online) c.fillCircle({av.right() - 3, av.bottom() - 3}, 4, col.success);
        const float x = av.right() + 12, right = r.right() - (remove_ ? 40 : 0);
        c.text(name_, {x, r.y + 8, right - x, 18}, col.fgPrimary.mulAlpha(alpha), gfx::VAlign::Center);
        c.text(status_, {x, r.y + 27, right - x, 16}, col.fgTertiary, gfx::VAlign::Center);
        paintChildren(c);
    }

private:
    static std::wstring statusOf(const together::MemberView& m) {
        std::wstring s = m.host ? std::wstring(tr(L"Oturum sahibi")) : std::wstring(tr(L"Misafir"));
        if (m.me) s += L" · " + std::wstring(tr(L"Sen"));
        if (!m.online) s += L" · " + std::wstring(tr(L"Çevrimdışı"));
        return s;
    }
    together::MemberView m_;
    gfx::Text name_, status_;
    Button* remove_ = nullptr;
};

// A toggle with a title and a description that both wrap (the panel is narrow for SettingRow's one-line title).
class ToggleRow : public ui::Widget {
public:
    ToggleRow(std::wstring title, std::wstring desc, bool on, std::function<void(bool)> onChange)
        : title_(std::move(title), type::body, {gfx::TextAlign::Leading, true, true, 3}),
          desc_(std::move(desc), type::caption, {gfx::TextAlign::Leading, true, true, 4}) {
        hitTestVisible = false;
        toggle_ = add<ui::Toggle>();
        toggle_->setOn(on, false);
        toggle_->onChange = std::move(onChange);
    }
    float preferredHeight(float w) override {
        const float tw = w - 40 - 16;
        return std::ceil(title_.measure(tw).h) + 4 + std::ceil(desc_.measure(tw).h) + 6;
    }
    void layout() override { toggle_->setRect({rect().w - 40, 0, 40, 22}); }
    void paint(Canvas& c) override {
        const Rect r = rect();
        const auto& col = colors();
        const float tw = r.w - 40 - 16;
        const float th = std::ceil(title_.measure(tw).h);
        c.text(title_, {r.x, r.y, tw, th + 2}, col.fgPrimary);
        c.text(desc_, {r.x, r.y + th + 4, tw, r.h - th - 4}, col.fgSecondary);
        paintChildren(c);
    }

private:
    gfx::Text title_, desc_;
    ui::Toggle* toggle_;
};

ui::Label* paragraph(ui::Column* c, const std::wstring& text, ui::Tone tone = ui::Tone::Secondary,
                     const gfx::TextStyle& style = type::secondary) {
    auto* l = c->add<ui::Label>(text, style, tone);
    l->setWrap(true);
    l->setVAlign(gfx::VAlign::Top);
    return l;
}

void sectionLabel(ui::Column* c, const std::wstring& text) {
    c->add<Spacer>(20.f);
    auto* l = c->add<ui::Label>(toUpperTr(text), type::monoLabel, ui::Tone::Tertiary);
    l->setVAlign(gfx::VAlign::Top);
    c->add<Spacer>(6.f);
}

} // namespace

JamPanel::JamPanel() : title_(tr(L"Birlikte dinle"), type::title) {
    clipsChildren = true;
    close_ = add<Button>(ButtonKind::Icon, L"", "close");
    close_->setIconSize(16);
    close_->setTooltip(tr(L"Kapat"));
    close_->onClick = [] { commands::run("listen-together"); };
    scroll_ = add<ui::ScrollView>();
    together::subscribe(life_.ref(), [this, ref = life_.ref()] {
        Dispatcher::post([this, ref] {
            if (!ref.expired()) rebuild();
        });
    });
    rebuild();
}

void JamPanel::rebuild() {
    auto* c = scroll_->setContent<ui::Column>(0.f, kPadX, 0.f, 24.f);
    const auto v = together::view();
    builtFor_ = v.role;
    switch (v.role) {
    case together::Role::None: {
        paragraph(c, tr(L"Arkadaşlarınla aynı şarkıyı aynı anda dinle. Katılanların Spotify hesabı olması gerekmez: "
                        L"şarkılar herkesin bilgisayarında aynı YouTube kaydından çalar."));
        c->add<Spacer>(16.f);
        auto* row = c->add<ButtonRow>();
        row->button(ButtonKind::Primary, tr(L"Oturum başlat"), "users", [] { together::startHosting(); });
        c->add<Spacer>(8.f);
        auto* join = c->add<ButtonRow>();
        join->button(ButtonKind::Secondary, tr(L"Davet bağlantısıyla katıl"), "link", [] { together::promptJoin(); });
        sectionLabel(c, tr(L"Nasıl çalışır"));
        paragraph(c, tr(L"Oturum sahibi çalmayı yönetir; misafirler şarkı ekleyebilir. En fazla 10 kişi katılabilir."),
                  ui::Tone::Tertiary);
        c->add<Spacer>(8.f);
        paragraph(c, tr(L"Her şey davet bağlantısındaki anahtarla uçtan uca şifrelenir: aradaki herkese açık MQTT "
                        L"sunucusu yalnızca şifreli veri görür. Bağlantıyı yalnızca güvendiğin kişilerle paylaş."),
                  ui::Tone::Tertiary);
        break;
    }
    case together::Role::Host: {
        c->add<StatusLine>();
        sectionLabel(c, tr(L"Davet bağlantısı"));
        c->add<InviteBox>(v.invite);
        c->add<Spacer>(10.f);
        auto* row = c->add<ButtonRow>();
        auto* copy = row->button(ButtonKind::Primary, tr(L"Bağlantıyı kopyala"), "link", [] { together::copyInvite(); });
        copy->setEnabled(!v.invite.empty());
        auto* fresh = row->button(ButtonKind::Ghost, tr(L"Yeni bağlantı"), "refresh", [] { together::rotateInvite(); });
        fresh->setTooltip(tr(L"Yeni davet bağlantısı: eski bağlantı çalışmaz olur, çevrimiçi misafirler yeni odaya taşınır"));
        c->add<Spacer>(8.f);
        paragraph(c, tr(L"Bağlantıya sahip olan herkes katılabilir. Arkadaşın bağlantıya tıklayabilir ya da ShadeTube'da "
                        L"aramaya yapıştırabilir."),
                  ui::Tone::Tertiary, type::caption);
        sectionLabel(c, i18n::format(tr(L"Katılımcılar · {}/{}"), {std::to_wstring(v.members.size()), L"10"}));
        for (const auto& m : v.members) c->add<MemberRow>(m, !m.host);
        if (v.members.size() <= 1)
            paragraph(c, tr(L"Henüz kimse katılmadı. Davet bağlantısını gönder; katılanlar burada görünür."), ui::Tone::Tertiary,
                      type::caption);
        sectionLabel(c, tr(L"İzinler"));
        c->add<ToggleRow>(tr(L"Misafirler çalmayı kontrol edebilir"), tr(L"Duraklatma, atlama ve sarma. Şarkı eklemek her zaman açık."),
                          v.guestsControl, [](bool on) { together::setGuestsControl(on); });
        c->add<Spacer>(24.f);
        auto* end = c->add<ButtonRow>();
        end->button(ButtonKind::Secondary, tr(L"Oturumu bitir"), "close", [] { together::leave(); });
        if (jam::brokerOrder("", Settings::get().jamBroker).size() > 1) {
            auto* move = end->button(ButtonKind::Ghost, tr(L"Başka sunucuya taşı"), "swap-match", [] { together::moveToNextBroker(); });
            move->setTooltip(tr(L"Sunucu yavaşsa ya da kesiliyorsa oturumu sıradaki herkese açık sunucuya taşır"));
        }
        break;
    }
    case together::Role::Guest: {
        c->add<StatusLine>();
        c->add<Spacer>(12.f);
        if (v.joining) {
            paragraph(c, tr(L"Oturuma bağlanılıyor…"), ui::Tone::Primary, type::body);
            c->add<Spacer>(6.f);
            paragraph(c, tr(L"Oturum sahibinin durumu bekleniyor. Bu birkaç saniye sürebilir."), ui::Tone::Tertiary, type::caption);
            c->add<Spacer>(16.f);
            auto* cancel = c->add<ButtonRow>();
            cancel->button(ButtonKind::Secondary, tr(L"Vazgeç"), "close", [] { together::leave(); });
            break;
        }
        paragraph(c, i18n::format(tr(L"{} ile birlikte dinliyorsun"), {v.hostName}), ui::Tone::Primary, type::body);
        c->add<Spacer>(8.f);
        c->add<SyncLine>();
        sectionLabel(c, i18n::format(tr(L"Katılımcılar · {}/{}"), {std::to_wstring(v.members.size()), L"10"}));
        for (const auto& m : v.members) c->add<MemberRow>(m, false);
        sectionLabel(c, tr(L"İzinler"));
        paragraph(c, v.guestsControl ? std::wstring(tr(L"Oturum sahibi misafirlerin de duraklatıp atlayabilmesine izin veriyor. "
                                                        L"Bir şarkıyı çaldığında ya da sıraya eklediğinde oturumun sırasına girer."))
                                     : std::wstring(tr(L"Çalmayı oturum sahibi kontrol eder. Bir şarkıyı çaldığında ya da sıraya "
                                                        L"eklediğinde oturumun sırasına girer.")),
                  ui::Tone::Tertiary, type::caption);
        c->add<Spacer>(20.f);
        auto* leave = c->add<ButtonRow>();
        leave->button(ButtonKind::Secondary, tr(L"Oturumdan ayrıl"), "logout", [] { together::leave(); });
        c->add<Spacer>(8.f);
        paragraph(c, tr(L"Ayrılınca kendi çalma sıran kaldığı yerden geri gelir."), ui::Tone::Tertiary, type::caption);
        break;
    }
    }
    scroll_->contentChanged();
    requestLayout();
    invalidate();
}

void JamPanel::layout() {
    const Rect r = rect();
    close_->setRect({r.w - kPadX - 32, 20 + 2, 32, 32});
    scroll_->setRect({0, kListTop, r.w, std::max(0.f, r.h - kListTop)});
}

bool JamPanel::onWheel(float dy, float dx, const ui::MouseEvent& e) { return scroll_->onWheel(dy, dx, e); }

void JamPanel::paint(Canvas& c) {
    const Rect r = rect();
    const auto& col = colors();
    c.fillRect(r, col.bgBase.withAlpha(0.35f));
    c.vline(r.x, r.y, r.bottom(), col.hairSubtle);
    c.text(title_, {r.x + kPadX, r.y + 20, r.w - kPadX * 2 - 40, 36}, col.fgPrimary, gfx::VAlign::Center);
    paintChildren(c);
    // The status / sync lines read the session live.
    if (together::active())
        if (auto* w = window()) w->invalidateAfter(1000);
}

// ---- Ayarlar › BİRLİKTE DİNLE -----------------------------------------------------------------------------------------

void buildJamRows(ui::Column* c, const std::function<void()>&) {
    auto& s = Settings::get();
    {
        auto* row = c->add<SettingRow>(tr(L"Görünen ad"),
                                       i18n::format(tr(L"Birlikte dinlerken diğerlerinin gördüğü ad. Boş bırakırsan \"{}\" kullanılır."),
                                                    {toWide(together::defaultName())}));
        auto* box = row->control<ui::TextBox>(220.f, ui::TextBox::Look::Field, toWide(together::defaultName()));
        box->setText(toWide(s.jamName));
        box->onChange = [](const std::wstring& text) {
            std::wstring t = text;
            if (t.size() > 40) t.resize(40);
            Settings::get().jamName = toUtf8(t);
            Settings::get().markDirty();
        };
    }
    {
        auto* row = c->add<SettingRow>(
            tr(L"Kendi MQTT sunucun"),
            tr(L"İsteğe bağlı: wss:// ile başlayan bir MQTT WebSocket adresi. Oturum başlatırken önce bu denenir, sonra herkese "
               L"açık sunucular (broker.emqx.io, test.mosquitto.org, broker.hivemq.com). Sunucu davet bağlantısının "
               L"içindedir; oturumdaki herkes onu kullanır."));
        auto* box = row->control<ui::TextBox>(260.f, ui::TextBox::Look::Field, L"wss://sunucu:8084/mqtt");
        box->setText(toWide(s.jamBroker));
        box->setError(!s.jamBroker.empty() && !jam::mqtt::parseBrokerUrl(s.jamBroker));
        box->onChange = [box](const std::wstring& text) {
            std::wstring t = text;
            while (!t.empty() && iswspace(t.back())) t.pop_back();
            while (!t.empty() && iswspace(t.front())) t.erase(t.begin());
            const std::string url = toUtf8(t);
            const bool ok = url.empty() || jam::mqtt::parseBrokerUrl(url).has_value();
            box->setError(!ok);
            if (!ok) return;   // kept as typed in the box; saved once it is a broker address
            Settings::get().jamBroker = url;
            Settings::get().markDirty();
        };
    }
    c->add<SettingRow>(tr(L"Gizlilik"),
                       tr(L"Oturumdaki her mesaj davet bağlantısındaki anahtarla uçtan uca şifrelenir; sunucu yalnızca şifreli "
                          L"veri ve rastgele bir konu adı görür. Bir oturumdan çıkarılan biri, yeni bir davet bağlantısı "
                          L"oluşturulduğunda geri dönemez."));
}

} // namespace st::app
