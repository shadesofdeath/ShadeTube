// Ayarlar › OYNATMA rows of the backup audio source (youtube/AltSource): Piped / Invidious, used only when
// YoutubeExplode cannot deliver a stream or a search; the chosen kind is asked first. SoundCloud (youtube/SoundCloud)
// is the last resort behind them.
#include "app/SettingsWidgets.h"
#include "core/I18n.h"
#include "core/Utf.h"
#include "ui/TextBox.h"
#include "youtube/AltSource.h"

namespace st::app {

namespace {

namespace alt = youtube::alt;

// The MatchService keeps its own thread-safe copy (worker threads read it); push every saved change to it.
void applyAltSource() {
    const auto& s = Settings::get();
    if (!ctx().matcher) return;
    ctx().matcher->setAltSource(s.altSource, s.altSourceInstance);
    ctx().matcher->setSoundCloud(s.altSoundCloud);
}

// While typing, only addresses that already look complete are applied (a dot or a port in the host); Enter applies
// any valid address and reports the result.
bool looksComplete(const std::string& normalized) {
    const std::string host = alt::hostOf(normalized);
    return host.find('.') != std::string::npos || host.find(':') != std::string::npos || host == "localhost";
}

} // namespace

void buildAltSourceRows(ui::Column* c, const std::function<void()>& rebuild) {
    const auto& s = Settings::get();
    const alt::Kind kind = alt::parseKind(s.altSource);
    auto* row = c->add<SettingRow>(tr(L"Yedek ses kaynağı"),
                                   tr(L"YouTube'dan ses alınamazsa önce seçtiğin türdeki, sonra diğer yedek sunucular "
                                      L"denenir. Bu sunucular hangi videoları dinlediğini görür."));
    const std::vector<std::wstring> kinds{tr(L"Kapalı"), L"Piped", L"Invidious"};   // brand names stay as they are
    // The control slot is exactly the pill's width, so it ends flush right like the toggles and the field below.
    const float segW = Segmented(kinds, 0).naturalWidth();
    auto* seg = row->control<Segmented>(segW, kinds, static_cast<int>(kind));
    seg->onChange = [rebuild](int i) {
        static const char* const kKinds[] = {"off", "piped", "invidious"};
        auto& st = Settings::get();
        const alt::Kind before = alt::parseKind(st.altSource);
        st.altSource = kKinds[i];
        st.markDirty();
        applyAltSource();
        // The address row exists only while a source is chosen, and its placeholder names the kind.
        if (alt::parseKind(st.altSource) != before) rebuild();
    };
    if (kind == alt::Kind::Off) return;

    auto* addrRow = c->add<SettingRow>(tr(L"Sunucu adresi"),
                                       tr(L"Boş bırakırsan yerleşik sunucu listesi sırayla denenir. Kendi sunucun "
                                          L"varsa adresini yaz (Piped için API adresi)."));
    // Example addresses: "ornek" (example) is a word, so a translation may use its own ("example.com").
    auto* box = addrRow->control<ui::TextBox>(320.f, ui::TextBox::Look::Field,
                                              kind == alt::Kind::Piped ? tr(L"https://pipedapi.ornek.com")
                                                                       : tr(L"https://invidious.ornek.com"));
    box->setText(toWide(s.altSourceInstance));
    auto apply = [](const std::wstring& raw, bool report) {
        const std::string text = toUtf8(raw);
        const bool blank = text.find_first_not_of(" \t") == std::string::npos;
        const auto normalized = alt::normalizeInstance(text);
        if (!blank && !normalized) {
            if (report)
                toast(tr(L"Geçersiz sunucu adresi: http(s) ile başlayan, boşluk, ? ya da # içermeyen bir adres yaz "
                         L"(ör. https://pipedapi.ornek.com)."),
                      true);
            return;
        }
        if (!blank && !report && !looksComplete(*normalized)) return;
        auto& st = Settings::get();
        const std::string value = normalized.value_or(std::string{});
        if (st.altSourceInstance != value) {
            st.altSourceInstance = value;
            st.markDirty();
            applyAltSource();
        }
        if (report)
            toast(value.empty() ? tr(L"Yerleşik sunucu listesi kullanılacak") : tr(L"Sunucu adresi kaydedildi"));
    };
    box->onChange = [apply](const std::wstring& v) { apply(v, false); };
    box->onSubmit = [apply](const std::wstring& v) { apply(v, true); };

    settingsToggle(c, tr(L"Son çare olarak SoundCloud"),
                   tr(L"YouTube ve yedek sunucular çalamazsa şarkı SoundCloud'da aranır. Yalnızca süresi tutan tam "
                      L"sürümler çalınır; SoundCloud ne dinlediğini görür."),
                   s.altSoundCloud, [](bool on) {
                       Settings::get().altSoundCloud = on;
                       applyAltSource();
                   });
}

} // namespace st::app
