// Ayarlar rows of the playback features: "Sonsuz çalma" (OYNATMA) and the KARA LİSTE section.
#include "app/Blacklist.h"
#include "app/Radio.h"
#include "app/SettingsWidgets.h"
#include "core/I18n.h"
#include "core/Utf.h"

namespace st::app {

void buildEndlessPlaybackRows(ui::Column* c, const std::function<void()>&) {
    settingsToggle(c, tr(L"Sonsuz çalma"),
                   tr(L"Sıra bitince çalan şarkılara benzer şarkılarla (Spotify radyosu) devam eder. Spotify "
                      L"bağlantısı gerekir."),
                   Settings::get().endlessPlayback, [](bool v) {
                       Settings::get().endlessPlayback = v;
                       if (v) endlessCheck();   // already on the last track: fill the queue now
                   });
}

void buildBlacklistSection(ui::Column* c, const std::function<void()>& rebuild) {
    const auto& artists = blacklist::blockedArtists();
    const auto& tracks = blacklist::blockedTracks();
    if (artists.empty() && tracks.empty()) {
        c->add<SettingRow>(tr(L"Kara liste boş"),
                           tr(L"Bir şarkıya sağ tıklayıp \"Bu şarkıyı engelle\" ya da \"Sanatçıyı engelle\" seç. "
                              L"Engellenenler sırada, karıştırmada, radyoda ve sonsuz çalmada atlanır; listede "
                              L"tıkladığın şarkı yine çalar."));
        return;
    }
    c->add<SettingRow>(tr(L"Engellenenler"),
                       tr(L"Sırada, karıştırmada, radyoda ve sonsuz çalmada atlanır; listelerde soluk görünür. Listede "
                          L"tıkladığın şarkı yine çalar."));
    auto removeButton = [](SettingRow* row) {
        return row->control<ui::Button>(96.f, ui::ButtonKind::Secondary, tr(L"Kaldır"));
    };
    // The description is a " · " list of facts: kind, [artists,] when ("Şarkı · Sanatçı · 3 gün önce engellendi").
    auto when = [](int64_t at) {
        const std::wstring rel = relativeTime(at);
        return rel.empty() ? std::wstring{} : L" · " + i18n::format(tr(L"{} engellendi"), {rel});
    };
    // "Kaldır" removes exactly that entry (no matching rules: another entry of a similar song stays).
    auto removeOnClick = [rebuild](const blacklist::Entry& e) {
        return [e, rebuild] {
            blacklist::remove(e);
            toast(i18n::format(tr(L"\"{}\" engeli kaldırıldı"), {toWide(e.name)}));
            rebuild();
        };
    };
    for (const auto& a : artists) {
        auto* row = c->add<SettingRow>(toWide(a.name), tr(L"Sanatçı") + when(a.addedAt));
        removeButton(row)->onClick = removeOnClick(a);
    }
    for (const auto& t : tracks) {
        std::wstring desc = tr(L"Şarkı");
        if (!t.artists.empty()) desc += L" · " + toWide(t.artists);
        desc += when(t.addedAt);
        auto* row = c->add<SettingRow>(toWide(t.name), desc);
        removeButton(row)->onClick = removeOnClick(t);
    }
}

} // namespace st::app
