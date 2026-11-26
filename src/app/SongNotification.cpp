#include "app/SongNotification.h"

#include <cwchar>

namespace st::app::songnotify {

Verdict decide(const Input& in) {
    if (!in.enabled) return Verdict::Off;
    if (!in.song) return Verdict::NotASong;
    if (!in.playing) return Verdict::Paused;
    if (in.repeat) return Verdict::Repeat;
    if (in.inView) return Verdict::InView;
    if (in.sinceMediaControlMs < kMediaControlMs) return Verdict::WindowsShows;
    if (in.guest && in.recentChanges >= kStormChanges) return Verdict::SyncStorm;
    if (in.busy) return Verdict::Busy;
    return Verdict::Show;
}

const char* name(Verdict v) {
    switch (v) {
    case Verdict::Show: return "show";
    case Verdict::Off: return "off";
    case Verdict::NotASong: return "not a song";
    case Verdict::Paused: return "paused";
    case Verdict::Repeat: return "same song";
    case Verdict::InView: return "a window is in view";
    case Verdict::WindowsShows: return "Windows' media overlay shows it";
    case Verdict::SyncStorm: return "listen-together sync burst";
    case Verdict::Busy: return "Windows says busy";
    }
    return "?";
}

std::wstring xmlEscape(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'&': out += L"&amp;"; break;
        case L'<': out += L"&lt;"; break;
        case L'>': out += L"&gt;"; break;
        case L'"': out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default:
            // Control characters are not allowed in XML 1.0 (a stray one in a tag would make the toast fail to load).
            if (c < 0x20 && c != L'\t' && c != L'\n' && c != L'\r') break;
            out += c;
        }
    }
    return out;
}

std::wstring fileUri(const std::filesystem::path& p) {
    std::wstring out = L"file:///";
    for (wchar_t c : p.wstring()) {
        if (c == L'\\') out += L'/';
        else if (c == L' ' || c == L'#' || c == L'%' || c == L'?') {
            wchar_t buf[4];
            std::swprintf(buf, 4, L"%%%02X", static_cast<unsigned>(c));
            out += buf;
        } else out += c;
    }
    return out;
}

std::wstring toastXml(const Content& c) {
    std::wstring x = L"<toast launch=\"open\" duration=\"short\"><visual><binding template=\"ToastGeneric\">";
    if (!c.image.empty()) x += L"<image placement=\"appLogoOverride\" src=\"" + xmlEscape(fileUri(c.image)) + L"\"/>";
    x += L"<text hint-maxLines=\"1\">" + xmlEscape(c.title) + L"</text>";
    if (!c.artist.empty()) x += L"<text hint-maxLines=\"1\">" + xmlEscape(c.artist) + L"</text>";
    if (!c.album.empty()) x += L"<text placement=\"attribution\">" + xmlEscape(c.album) + L"</text>";
    x += L"</binding></visual><actions>";
    x += L"<action content=\"" + xmlEscape(c.nextLabel) + L"\" arguments=\"next\" activationType=\"foreground\"/>";
    if (c.likeButton) x += L"<action content=\"" + xmlEscape(c.likeLabel) + L"\" arguments=\"like\" activationType=\"foreground\"/>";
    x += L"</actions><audio silent=\"true\"/></toast>";
    return x;
}

} // namespace st::app::songnotify
