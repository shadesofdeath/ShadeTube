// Listen together: invite links, topics and the shadetube: URL protocol (see JamInvite.h).
#include "app/JamInvite.h"

#include "app/JamMqtt.h"

#include <windows.h>

#include <algorithm>

namespace st::app::jam {

namespace {

constexpr uint8_t kVersion = 1;
constexpr uint8_t kFlagPlainWs = 0x01;
constexpr size_t kHeader = 2 + kRoomIdBytes + kKeyBytes;
constexpr size_t kMaxBroker = 200;

std::string_view trim(std::string_view s) {
    constexpr std::string_view space = " \t\r\n\f\v";
    const size_t b = s.find_first_not_of(space);
    if (b == std::string_view::npos) return {};
    return s.substr(b, s.find_last_not_of(space) - b + 1);
}

bool startsWithNoCase(std::string_view s, std::string_view prefix) {
    if (s.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        char a = s[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (a != prefix[i]) return false;
    }
    return true;
}

std::wstring wenv(const wchar_t* name) {
    wchar_t buf[1024];
    const DWORD n = GetEnvironmentVariableW(name, buf, 1024);
    return n > 0 && n < 1024 ? std::wstring(buf, n) : std::wstring();
}

std::wstring regString(HKEY root, const std::wstring& sub, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes < sizeof(wchar_t))
        return {};
    std::wstring s(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, sub.c_str(), name, RRF_RT_REG_SZ, nullptr, s.data(), &bytes) != ERROR_SUCCESS) return {};
    s.resize(wcsnlen(s.c_str(), s.size()));
    return s;
}

bool regSet(const std::wstring& sub, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sub.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return false;
    const LSTATUS st = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                      static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return st == ERROR_SUCCESS;
}

} // namespace

const std::vector<std::string>& defaultBrokers() {
    static const std::vector<std::string> list{
        "wss://broker.emqx.io:8084/mqtt",
        "wss://test.mosquitto.org:8081/mqtt",
        "wss://broker.hivemq.com:8884/mqtt",
    };
    return list;
}

std::vector<std::string> brokerOrder(const std::string& first, const std::string& custom) {
    std::vector<std::string> out;
    auto add = [&](const std::string& url) {
        if (url.empty() || !mqtt::parseBrokerUrl(url) || std::find(out.begin(), out.end(), url) != out.end()) return;
        out.push_back(url);
    };
    add(first);
    add(custom);
    for (const auto& b : defaultBrokers()) add(b);
    return out;
}

Invite newInvite(std::string broker) {
    Invite i;
    i.roomId = randomBytes(kRoomIdBytes);
    i.key = randomBytes(kKeyBytes);
    i.broker = std::move(broker);
    return i;
}

std::string formatInvite(const Invite& invite) {
    if (invite.roomId.size() != kRoomIdBytes || invite.key.size() != kKeyBytes) return {};
    const auto url = mqtt::parseBrokerUrl(invite.broker);
    if (!url) return {};
    std::string rest = invite.broker.substr(url->secure ? 6 : 5);   // past "wss://" / "ws://"
    if (rest.empty() || rest.size() > kMaxBroker) return {};
    Bytes p;
    p.reserve(kHeader + rest.size());
    p.push_back(kVersion);
    p.push_back(url->secure ? 0 : kFlagPlainWs);
    p.insert(p.end(), invite.roomId.begin(), invite.roomId.end());
    p.insert(p.end(), invite.key.begin(), invite.key.end());
    p.insert(p.end(), rest.begin(), rest.end());
    return std::string(kInvitePrefix) + base64url(p);
}

bool looksLikeInvite(std::string_view text) {
    std::string_view s = trim(text);
    while (s.size() >= 2 && ((s.front() == '<' && s.back() == '>') || (s.front() == '"' && s.back() == '"') ||
                             (s.front() == '\'' && s.back() == '\'')))
        s = trim(s.substr(1, s.size() - 2));
    return startsWithNoCase(s, kInvitePrefix);
}

std::optional<Invite> parseInvite(std::string_view text) {
    std::string_view s = trim(text);
    while (s.size() >= 2 && ((s.front() == '<' && s.back() == '>') || (s.front() == '"' && s.back() == '"') ||
                             (s.front() == '\'' && s.back() == '\'')))
        s = trim(s.substr(1, s.size() - 2));
    if (!startsWithNoCase(s, kInvitePrefix)) return std::nullopt;
    s.remove_prefix(kInvitePrefix.size());
    while (!s.empty() && s.back() == '/') s.remove_suffix(1);
    if (s.empty() || s.size() > 512) return std::nullopt;
    const auto bytes = fromBase64url(s);
    if (!bytes || bytes->size() <= kHeader || bytes->size() > kHeader + kMaxBroker || (*bytes)[0] != kVersion) return std::nullopt;
    const uint8_t flags = (*bytes)[1];
    if (flags & ~kFlagPlainWs) return std::nullopt;
    Invite i;
    i.roomId.assign(bytes->begin() + 2, bytes->begin() + 2 + kRoomIdBytes);
    i.key.assign(bytes->begin() + 2 + kRoomIdBytes, bytes->begin() + kHeader);
    const std::string rest(bytes->begin() + kHeader, bytes->end());
    if (!std::all_of(rest.begin(), rest.end(), [](char c) { return c > 0x20 && c < 0x7F; })) return std::nullopt;
    i.broker = std::string((flags & kFlagPlainWs) ? "ws://" : "wss://") + rest;
    if (!mqtt::parseBrokerUrl(i.broker)) return std::nullopt;
    return i;
}

std::string topicBase(const Bytes& roomId) { return "shadetube/jam/v1/" + toHex(sha256(roomId)); }

std::string roomTag(const Bytes& roomId) { return toHex(sha256(roomId)).substr(0, 8); }

// ---- URL protocol ---------------------------------------------------------------------------------------------------

bool protocolKeyOverridden() { return !wenv(L"SHADETUBE_PROTOCOL_KEY").empty(); }

std::wstring protocolKeyPath() {
    std::wstring k = wenv(L"SHADETUBE_PROTOCOL_KEY");
    if (k.empty()) return L"Software\\Classes\\shadetube";
    // Always HKCU: a hive prefix is stripped; a malformed override never falls back to the real key.
    if (const auto slash = k.find(L'\\'); slash != std::wstring::npos && k.rfind(L"HK", 0) == 0) k.erase(0, slash + 1);
    while (!k.empty() && k.back() == L'\\') k.pop_back();
    return k.empty() ? L"Software\\ShadeTubeTest\\InvalidProtocolKey" : k;
}

std::wstring protocolCommand(const std::filesystem::path& exe) { return L"\"" + exe.wstring() + L"\" --open-link \"%1\""; }

std::wstring registeredCommand(const std::wstring& keyPath) {
    return regString(HKEY_CURRENT_USER, keyPath + L"\\shell\\open\\command", nullptr);
}

bool registerUrlProtocol(const std::filesystem::path& exe, const std::wstring& keyPath) {
    const std::wstring command = protocolCommand(exe);
    const std::wstring icon = L"\"" + exe.wstring() + L"\",0";
    if (registeredCommand(keyPath) == command && regString(HKEY_CURRENT_USER, keyPath, L"URL Protocol").empty() &&
        regString(HKEY_CURRENT_USER, keyPath, nullptr) == L"URL:ShadeTube Protocol" &&
        regString(HKEY_CURRENT_USER, keyPath + L"\\DefaultIcon", nullptr) == icon) {
        // "URL Protocol" must exist (empty value): RegGetValue of an empty REG_SZ gives "", like a missing one.
        HKEY k = nullptr;
        bool has = false;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath.c_str(), 0, KEY_QUERY_VALUE, &k) == ERROR_SUCCESS) {
            has = RegQueryValueExW(k, L"URL Protocol", nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
            RegCloseKey(k);
        }
        if (has) return false;
    }
    bool ok = regSet(keyPath, nullptr, L"URL:ShadeTube Protocol");
    ok = regSet(keyPath, L"URL Protocol", L"") && ok;
    ok = regSet(keyPath + L"\\DefaultIcon", nullptr, icon) && ok;
    ok = regSet(keyPath + L"\\shell\\open\\command", nullptr, command) && ok;
    return ok;
}

bool unregisterUrlProtocol(const std::wstring& keyPath) {
    if (keyPath.empty()) return false;
    const LSTATUS st = RegDeleteTreeW(HKEY_CURRENT_USER, keyPath.c_str());
    return st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND;
}

} // namespace st::app::jam
