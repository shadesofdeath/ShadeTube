#pragma once
// Listen together ("Birlikte dinle", app/Together): the invite link, the room's topics and the `shadetube:` URL
// protocol.
//
// Invite link: shadetube://jam/<base64url payload>, payload = version 1 | flags | room id (16) | key (32) | broker.
// The broker is "host[:port][/path]" in UTF-8 (wss:// is implied; flag bit 0 = plain ws://, for a broker on the local
// network). The room id names the topics (shadetube/jam/v1/<hex SHA-256(room id)>/...: the broker never sees the
// room id itself) and the key seals every message (AES-256-GCM, app/JamCrypto). Whoever has the link is in the room;
// it is never logged.
//
// URL protocol: HKCU\Software\Classes\shadetube (URL:ShadeTube Protocol), shell\open\command =
// "<exe>" --open-link "%1": a clicked link starts ShadeTube, or main.cpp hands it to the running instance.
//
// Standalone (tests/jam compiles it): app/JamCrypto, app/JamMqtt (broker URLs) and the Windows SDK.
#include "app/JamCrypto.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace st::app::jam {

inline constexpr std::string_view kInvitePrefix = "shadetube://jam/";
inline constexpr size_t kRoomIdBytes = 16;

struct Invite {
    Bytes roomId;          // 16 bytes
    Bytes key;             // 32 bytes
    std::string broker;    // wss://host[:port][/path] (or ws://)
    bool operator==(const Invite&) const = default;
};

// Public brokers, best first (checked 2026-11-11 from Windows/WinHTTP: EMQX and Mosquitto's test broker answer over
// wss; HiveMQ's public broker is kept last as it was unreachable that day).
const std::vector<std::string>& defaultBrokers();
// The order a session tries brokers in: `first` (the invite's / the current one), `custom` (Settings::jamBroker), then
// the defaults; duplicates and invalid URLs dropped.
std::vector<std::string> brokerOrder(const std::string& first, const std::string& custom);

Invite newInvite(std::string broker);            // fresh random room id + key
std::string formatInvite(const Invite& invite);  // "" when the invite is invalid (sizes, broker URL)
// Accepts the link with surrounding spaces / quotes / <...>, a trailing '/' (browsers add one) and any case of the
// scheme. nullopt for anything else (wrong version, sizes, a broken broker URL, bad base64url).
std::optional<Invite> parseInvite(std::string_view text);
// Cheap check for the paste paths (app/Links): starts like an invite link. parseInvite() decides.
bool looksLikeInvite(std::string_view text);

// "shadetube/jam/v1/<64 hex>" of a room; its topics are "<base>/state" (retained, host), "<base>/h" (host) and
// "<base>/g" (guests).
std::string topicBase(const Bytes& roomId);
// The room's short fingerprint for logs (first 8 hex digits of the topic hash; reveals nothing about the key).
std::string roomTag(const Bytes& roomId);

// ---- A link for the running instance --------------------------------------------------------------------------------
// A second launch (`ShadeTube.exe --open-link <link>`, e.g. a clicked shadetube: link) hands the link to the running
// ShadeTube as WM_COPYDATA (dwData = kOpenLinkCopyData, UTF-16 text without the terminator) and exits.
inline constexpr unsigned long kOpenLinkCopyData = 0x4A414D31;   // 'JAM1'
// Sends `link` to the main window ("ShadeTube") of `windowClass`, else any window of it. False: none answered.
bool forwardOpenLink(const wchar_t* windowClass, std::wstring_view link);
// The text of such a WM_COPYDATA (lParam), "" for anything else (or an oversized one).
std::wstring openLinkFromCopyData(const void* copyDataStruct);

// ---- URL protocol ---------------------------------------------------------------------------------------------------
// The HKCU sub key (under HKEY_CURRENT_USER) the protocol lives in: "Software\Classes\shadetube", or
// SHADETUBE_PROTOCOL_KEY (tests; any HKCU path, a "HKCU\" prefix is stripped).
std::wstring protocolKeyPath();
bool protocolKeyOverridden();
std::wstring protocolCommand(const std::filesystem::path& exe);   // "<exe>" --open-link "%1"
// Writes the protocol for `exe` under HKCU\<keyPath> unless it already says exactly that. True when written.
bool registerUrlProtocol(const std::filesystem::path& exe, const std::wstring& keyPath);
// The command the key holds now ("" = not registered).
std::wstring registeredCommand(const std::wstring& keyPath);
bool unregisterUrlProtocol(const std::wstring& keyPath);

} // namespace st::app::jam
