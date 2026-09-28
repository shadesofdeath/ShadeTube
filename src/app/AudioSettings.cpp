// Ayarlar > SES: loudness normalisation, crossfade, the 10-band equalizer (presets + a response graph you drag) and
// the output device. Everything applies live: the equalizer and the device through Player::applyAudioSettings(),
// normalisation and crossfade from the next track the player loads or preloads.
#include "app/SettingsWidgets.h"
#include "audio/AudioEngine.h"
#include "core/Async.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "player/Player.h"
#include "ui/Popups.h"
#include "ui/Window.h"

#include <array>
#include <cmath>

namespace st::app {

namespace {

using audio::kEqBands;

void applyAudio() {
    Settings::get().markDirty();
    if (auto* p = ctx().player) p->applyAudioSettings();
}

std::array<float, kEqBands> savedGains() {
    std::array<float, kEqBands> g{};
    const auto& s = Settings::get().eqGains;
    for (size_t i = 0; i < g.size() && i < s.size(); ++i) g[i] = s[i];
    return g;
}

void saveGains(const std::array<float, kEqBands>& g) {
    auto& s = Settings::get();
    s.eqGains.assign(g.begin(), g.end());
    s.eqPreset = "custom";
    for (const auto& p : audio::eqPresets())
        if (p.gainsDb == g) s.eqPreset = p.id;
}

// Preset names (the ids are stored in Settings::eqPreset).
std::wstring presetName(std::string_view id) {
    if (id == "flat") return tr(L"Düz");
    if (id == "bass") return tr(L"Bas güçlendirme");
    if (id == "bass-cut") return tr(L"Bas azaltma");
    if (id == "treble") return tr(L"Tiz güçlendirme");
    if (id == "treble-cut") return tr(L"Tiz azaltma");
    if (id == "vocal") return tr(L"Vokal");
    if (id == "pop") return L"Pop";            // genre names that read the same everywhere
    if (id == "rock") return L"Rock";
    if (id == "hiphop") return L"Hip hop";
    if (id == "electronic") return tr(L"Elektronik");
    if (id == "jazz") return tr(L"Caz");
    if (id == "classical") return tr(L"Klasik");
    if (id == "acoustic") return tr(L"Akustik");
    if (id == "loudness") return tr(L"Gece dinlemesi");
    if (id == "speech") return tr(L"Konuşma");
    return tr(L"Özel");
}

// "+3,5" / "-2" / "0" (dB, 0.5 steps) with the UI language's decimal separator.
std::wstring dbText(float db) {
    const float r = std::round(db * 2) / 2;
    if (r == 0) return L"0";
    wchar_t b[16];
    swprintf(b, 16, r == std::floor(r) ? L"%+.0f" : L"%+.1f", r);
    std::wstring s = b;
    std::replace(s.begin(), s.end(), L'.', i18n::decimalSeparator());
    return s;
}

// The equalizer: a preamp column and ten band columns over a live response curve. Drag a handle (or use the wheel),
// double-click to reset it. Keyboard: Left / Right pick a column, Up / Down 0.5 dB, PageUp / PageDown 3 dB, Delete /
// Backspace / 0 back to 0 dB. Disabled (dimmed, no input) while the equalizer is off.
class EqualizerView : public ui::Widget {
public:
    EqualizerView() { focusable = true; }
    std::function<void()> onChange;   // after Settings (and the engine) took a new value

    float preferredHeight(float) override { return 252; }
    bool activatable() const override { return true; }
    Rect focusRect() const override {
        const Rect p = plot();
        const float x = columnX(selected_);
        return {x - 14, p.y - 10, 28, p.h + 20};
    }
    ui::FocusShape focusShape() const override { return ui::FocusShape::Pill; }
    LPCWSTR cursor() const override { return enabled() && hot_ >= 0 ? IDC_SIZENS : IDC_ARROW; }

    bool onMouseDown(const ui::MouseEvent& e) override {
        if (!enabled()) return false;
        const int col = columnAt(e.pos.x);
        if (col < 0) return false;
        selected_ = col;
        if (e.clicks == 2) {
            setValue(col, 0);
            return true;
        }
        dragging_ = col;
        setValue(col, dbAt(e.pos.y));
        return true;
    }
    void onMouseMove(const ui::MouseEvent& e) override {
        if (dragging_ >= 0) {
            setValue(dragging_, dbAt(e.pos.y));
            return;
        }
        const int col = enabled() ? columnAt(e.pos.x) : -1;
        if (col != hot_) {
            hot_ = col;
            invalidate();
        }
    }
    void onMouseUp(const ui::MouseEvent&) override {
        dragging_ = -1;
        invalidate();
    }
    void onMouseLeave() override {
        hot_ = -1;
        invalidate();
    }
    bool onWheel(float dy, float, const ui::MouseEvent&) override {
        if (!enabled() || hot_ < 0) return false;
        setValue(hot_, value(hot_) + (dy > 0 ? 0.5f : -0.5f));
        return true;
    }
    bool onKeyDown(const ui::KeyEvent& e) override {
        if (!enabled() || e.ctrl || e.alt) return false;
        switch (e.vk) {
        case VK_LEFT: select(selected_ - 1); return true;
        case VK_RIGHT: select(selected_ + 1); return true;
        case VK_UP: setValue(selected_, value(selected_) + 0.5f); return true;
        case VK_DOWN: setValue(selected_, value(selected_) - 0.5f); return true;
        case VK_PRIOR: setValue(selected_, value(selected_) + 3); return true;
        case VK_NEXT: setValue(selected_, value(selected_) - 3); return true;
        case VK_DELETE:
        case VK_BACK:
        case '0':
        case VK_NUMPAD0: setValue(selected_, 0); return true;
        default: return false;
        }
    }
    void onFocusChanged(bool) override { invalidate(); }

    void paint(Canvas& c) override {
        const auto& col = gfx::colors();
        const Color acc = gfx::accent().base;
        const bool on = enabled();
        const Rect p = plot();
        if (!on) c.pushOpacity(0.45f);

        // Grid: +12 / +6 / 0 / -6 / -12 dB, labelled on the right.
        for (float db : {12.f, 6.f, 0.f, -6.f, -12.f}) {
            const float y = yFor(db);
            c.hline(p.x, p.right(), y, db == 0 ? col.hairDefault : col.hairSubtle);
            c.text(db == 0 ? L"0 dB" : dbText(db), gfx::type::monoMeta, {p.right() + 8, y - 8, 48, 16}, col.fgTertiary,
                   gfx::TextAlign::Leading, gfx::VAlign::Center);
        }
        const float split = (columnX(0) + columnX(1)) * 0.5f;
        c.vline(split, p.y, p.bottom(), col.hairSubtle);

        // Response of the bands (+ preamp), log-frequency axis aligned with the band columns.
        const auto gains = savedGains();
        const float preamp = Settings::get().eqPreampDb;
        const float x0 = columnX(1), x1 = columnX(kEqBands);
        const double l0 = std::log2(audio::kEqFrequencies.front()), l1 = std::log2(audio::kEqFrequencies.back());
        const float from = split + 6, to = p.right();
        gfx::Point prev{};
        for (float x = from; x <= to + 0.5f; x += 3) {
            const double f = std::exp2(l0 + (x - x0) / (x1 - x0) * (l1 - l0));
            const float db = static_cast<float>(audio::eqResponseDb(gains, std::clamp(f, 20.0, 20000.0))) + preamp;
            const gfx::Point pt{x, yFor(std::clamp(db, -audio::kEqMaxGainDb, audio::kEqMaxGainDb))};
            if (x > from) c.line(prev, pt, on ? acc : col.fgTertiary, 2.f);
            prev = pt;
        }

        // Columns: track, handle, value while hot / dragged / keyboard-selected, frequency label.
        static const wchar_t* kLabels[kEqBands] = {L"31", L"62", L"125", L"250", L"500", L"1K", L"2K", L"4K", L"8K", L"16K"};
        for (int i = 0; i <= kEqBands; ++i) {
            const float x = columnX(i);
            const float v = value(i);
            const bool active = i == dragging_ || (i == hot_ && dragging_ < 0) || (keyboardFocused() && i == selected_);
            c.vline(x, yFor(audio::kEqMaxGainDb), yFor(-audio::kEqMaxGainDb), active ? col.hairStrong : col.hairDefault, 2.f);
            const gfx::Point h{x, yFor(v)};
            c.fillCircle(h, active ? 8.f : 7.f, on ? (i == 0 ? col.fgPrimary : acc) : col.fgDisabled);
            if (active) c.text(dbText(v), gfx::type::monoMeta, {x - 30, p.y - 26, 60, 16}, col.fgPrimary, gfx::TextAlign::Center);
            const std::wstring label = i == 0 ? toUpperTr(tr(L"Ön yük.")) : kLabels[i - 1];
            c.text(label, gfx::type::monoMeta, {x - 30, p.bottom() + 12, 60, 16}, i == 0 ? col.fgSecondary : col.fgTertiary,
                   gfx::TextAlign::Center);
        }
        if (!on) c.popLayer();
        const Rect r = rect();
        c.hline(r.x, r.right(), r.bottom() - 1, col.hairSubtle);
    }

private:
    Rect plot() const {
        const Rect r = rect();
        return {r.x + 12, r.y + 40, std::max(40.f, r.w - 12 - 56), r.h - 40 - 52};
    }
    // Column 0 is the preamp (at the left, apart); 1..10 the bands, spread evenly.
    float columnX(int col) const {
        const Rect p = plot();
        if (col <= 0) return p.x + 22;
        const float a = p.x + 84, b = p.right() - 22;
        return a + (b - a) * static_cast<float>(col - 1) / (kEqBands - 1);
    }
    int columnAt(float x) const {
        int best = -1;
        float bestD = 1e9f;
        for (int i = 0; i <= kEqBands; ++i) {
            const float d = std::fabs(x - columnX(i));
            if (d < bestD) {
                bestD = d;
                best = i;
            }
        }
        return bestD <= 24 ? best : -1;
    }
    float yFor(float db) const {
        const Rect p = plot();
        return p.y + (audio::kEqMaxGainDb - db) / (2 * audio::kEqMaxGainDb) * p.h;
    }
    float dbAt(float y) const {
        const Rect p = plot();
        return audio::kEqMaxGainDb - (y - p.y) / p.h * (2 * audio::kEqMaxGainDb);
    }
    float value(int col) const { return col == 0 ? Settings::get().eqPreampDb : savedGains()[col - 1]; }
    void select(int col) {
        selected_ = std::clamp(col, 0, kEqBands);
        scrollIntoView();
        invalidate();
    }
    void setValue(int col, float db) {
        db = std::clamp(std::round(db * 2) / 2, -audio::kEqMaxGainDb, audio::kEqMaxGainDb);
        if (db == value(col)) return;
        if (col == 0) {
            Settings::get().eqPreampDb = db;
        } else {
            auto g = savedGains();
            g[col - 1] = db;
            saveGains(g);
        }
        applyAudio();
        invalidate();
        if (onChange) onChange();
    }

    int hot_ = -1, dragging_ = -1, selected_ = 1;
};

// Crossfade length: a slider (0 = off .. 12 s) with its value.
class CrossfadeControl : public ui::Widget {
public:
    CrossfadeControl() {
        slider_ = add<ui::Slider>(ui::Slider::Look::Plain);
        slider_->keyStep = 1.f / 12;
        slider_->setValue(Settings::get().crossfadeSec / 12.f);
        slider_->onScrub = [this](float v) { set(v); };
        slider_->onCommit = [this](float v) { set(v); };
        label_ = add<ui::Label>(text(Settings::get().crossfadeSec), gfx::type::monoMeta, ui::Tone::Secondary);
        label_->setAlign(gfx::TextAlign::Trailing);
    }
    void layout() override {
        const Rect r = rect();
        slider_->setRect({0, r.h * 0.5f - 10, r.w - 64, 20});
        label_->setRect({r.w - 56, 0, 56, r.h});
    }

private:
    static std::wstring text(int sec) { return sec <= 0 ? tr(L"Kapalı") : i18n::format(tr(L"{} sn"), {std::to_wstring(sec)}); }
    void set(float v) {
        const int sec = static_cast<int>(std::lround(std::clamp(v, 0.f, 1.f) * 12));
        auto& s = Settings::get();
        if (sec == s.crossfadeSec) return;
        s.crossfadeSec = sec;
        s.markDirty();
        label_->setText(text(sec));
    }
    ui::Slider* slider_;
    ui::Label* label_;
};

// The output device picker: the list is enumerated (on a worker) when the menu opens.
class DevicePicker : public ui::Button {
public:
    explicit DevicePicker(std::function<void()> rebuild)
        : ui::Button(ui::ButtonKind::Secondary, currentName(), "chevron-down"), rebuild_(std::move(rebuild)) {
        onClick = [this] { open(); };
    }

private:
    static std::wstring currentName() {
        const auto& s = Settings::get();
        if (s.outputDeviceId.empty()) return tr(L"Windows varsayılanı");
        return s.outputDeviceName.empty() ? tr(L"Seçili aygıt") : toWide(s.outputDeviceName);
    }
    void open() {
        async(Priority::High, life_.ref(), [] { return audio::AudioEngine::outputDevices(); },
              [this](Result<std::vector<audio::OutputDevice>> r) {
                  const std::string cur = Settings::get().outputDeviceId;
                  auto choose = [rebuild = rebuild_](std::string id, std::string name) {
                      return [rebuild, id = std::move(id), name = std::move(name)] {
                          auto& s = Settings::get();
                          s.outputDeviceId = id;
                          s.outputDeviceName = name;
                          applyAudio();
                          rebuild();
                      };
                  };
                  std::vector<ui::MenuItem> items;
                  ui::MenuItem def{tr(L"Windows varsayılanı"), "volume-2", L"", choose("", "")};
                  def.checked = cur.empty();
                  items.push_back(std::move(def));
                  if (r && !r->empty()) {
                      items.push_back(ui::MenuItem::sep());
                      for (const auto& d : *r) {
                          ui::MenuItem it{d.name, "", d.isDefault ? tr(L"varsayılan") : L"", choose(toUtf8(d.id), toUtf8(d.name))};
                          it.checked = !cur.empty() && cur == toUtf8(d.id);
                          items.push_back(std::move(it));
                      }
                  }
                  const Rect b = toWindow(rect());
                  ui::Menu::open(ctx().window, {b.x, b.bottom() + 4}, std::move(items));
              });
    }
    std::function<void()> rebuild_;
    Lifetime life_;
};

} // namespace

void buildAudioRows(ui::Column* c, const std::function<void()>& rebuild) {
    auto& s = Settings::get();

    // Loudness normalisation.
    settingsToggle(c, tr(L"Ses seviyesini dengele"),
                   tr(L"Şarkılar arasındaki ses farkını giderir: YouTube'un ölçtüğü ses yüksekliği ve yerel "
                      L"dosyalardaki ReplayGain etiketleri kullanılır. Bir sonraki şarkıdan itibaren uygulanır."),
                   s.normalizeVolume, [](bool v) { Settings::get().normalizeVolume = v; });
    {
        auto* row = c->add<SettingRow>(tr(L"Hedef ses seviyesi"),
                                       tr(L"Sessiz: -19 LUFS, Normal: -14 LUFS (Spotify ve YouTube ile aynı), Yüksek sesli: "
                                          L"-11 LUFS. Yükseltilen şarkıların tepeleri yumuşak bir sınırlayıcıyla korunur."));
        const int cur = s.loudnessTarget <= -17 ? 0 : s.loudnessTarget >= -12 ? 2 : 1;
        auto* seg = row->control<Segmented>(240.f, std::vector<std::wstring>{tr(L"Sessiz"), tr(L"Normal"), tr(L"Yüksek sesli")}, cur);
        seg->onChange = [](int i) {
            static const int kTargets[] = {-19, -14, -11};
            Settings::get().loudnessTarget = kTargets[i];
            Settings::get().markDirty();
        };
    }

    // Crossfade.
    {
        auto* row = c->add<SettingRow>(tr(L"Şarkılar arası geçiş"),
                                       tr(L"Şarkı bitmeden bu kadar önce sıradaki başlar ve ikisi yumuşakça karışır. "
                                          L"Sırayla çalan bir albümün şarkıları kesintisiz geçer."));
        row->control<CrossfadeControl>(260.f);
    }

    // Equalizer.
    auto* eqToggle = settingsToggle(c, tr(L"Ekolayzer"),
                                    tr(L"10 bantlı ekolayzer; çalan her şeye uygulanır. Yükseltilen bantlar sesi "
                                       L"kırpmasın diye genel seviye kendiliğinden düşürülür."),
                                    s.eqEnabled, [](bool) {});
    ui::Button* preset = nullptr;
    {
        auto* row = c->add<SettingRow>(tr(L"Hazır ayar"),
                                       tr(L"Bir başlangıç noktası seç; bantları sürükleyerek ya da tekerlekle ince ayar "
                                          L"yapabilirsin. Çift tıklama bandı sıfırlar."));
        preset = row->control<ui::Button>(200.f, ui::ButtonKind::Secondary, presetName(s.eqPreset), "chevron-down");
        preset->onClick = [preset, rebuild] {
            const std::string cur = Settings::get().eqPreset;
            std::vector<ui::MenuItem> items;
            for (const auto& p : audio::eqPresets()) {
                ui::MenuItem it{presetName(p.id), "", L"", [id = std::string(p.id), gains = p.gainsDb, rebuild] {
                                    auto& st = Settings::get();
                                    st.eqGains.assign(gains.begin(), gains.end());
                                    st.eqPreset = id;
                                    st.eqEnabled = true;   // picking a sound means hearing it
                                    applyAudio();
                                    rebuild();
                                }};
                it.checked = cur == p.id;
                items.push_back(std::move(it));
            }
            items.push_back(ui::MenuItem::sep());
            items.push_back({tr(L"Ön yükseltmeyi sıfırla"), "refresh", L"", [rebuild] {
                                 Settings::get().eqPreampDb = 0;
                                 applyAudio();
                                 rebuild();
                             }});
            const Rect b = preset->toWindow(preset->rect());
            ui::Menu::open(ctx().window, {b.x, b.bottom() + 4}, std::move(items));
        };
    }
    auto* view = c->add<EqualizerView>();
    view->setEnabled(s.eqEnabled);
    view->onChange = [preset] { preset->setLabel(presetName(Settings::get().eqPreset)); };
    eqToggle->onChange = [view](bool v) {
        Settings::get().eqEnabled = v;
        view->setEnabled(v);
        applyAudio();
    };

    // Output device.
    {
        auto* row = c->add<SettingRow>(tr(L"Çıkış aygıtı"),
                                       tr(L"Sesin çalınacağı aygıt. Seçtiğin aygıt çıkarılırsa Windows varsayılanına "
                                          L"geçilir ve geri takılınca ona dönülür."));
        row->control<DevicePicker>(240.f, rebuild);
    }
}

} // namespace st::app
