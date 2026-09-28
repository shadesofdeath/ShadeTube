#pragma once
// Discord Rich Presence over the local IPC pipe (\\.\pipe\discord-ipc-0 .. 9), no Discord SDK.
//
// Frames are int32 opcode + int32 length (little endian) + UTF-8 JSON. Handshake: op 0 {v:1, client_id};
// then op 1 {cmd:"SET_ACTIVITY", args:{pid, activity}, nonce}; activity null clears the presence.
//
// Everything runs on one private thread with a "latest state" slot: callers only store the wanted state and
// return immediately (never block, never throw); intermediate states are coalesced. The pipe is connected
// lazily (only when there is something to show), retried with backoff (5 s doubling to 60 s) and stays a
// silent no-op while Discord is not running. Updates are throttled to Discord's limit (5 per 20 s).
// The destructor stops the thread promptly (all pipe I/O is overlapped and cancellable); closing the pipe
// makes Discord drop the presence.
//
// Thread-safe. Standalone (no app headers). Empty application id = disabled.
#include <cstdint>
#include <memory>
#include <string>

namespace st::app {

class DiscordRpc {
public:
    DiscordRpc();
    // Test seam: base name of the pipes to try (the index 0..9 is appended). Default "\\.\pipe\discord-ipc-".
    explicit DiscordRpc(std::wstring pipeBaseName);
    ~DiscordRpc();
    DiscordRpc(const DiscordRpc&) = delete;
    DiscordRpc& operator=(const DiscordRpc&) = delete;

    // Discord application (client) id from discord.com/developers/applications. Empty = disabled (disconnects).
    void setAppId(std::string clientId);

    // "Listening to <app name>": details = title, state = artist, large image = cover URL (https), large text =
    // album. Timestamps are unix milliseconds (0 = omit); start+end give Discord's progress bar. Repeated calls
    // with the same text and timestamps within 2 s are ignored, so calling this every tick is cheap.
    void setActivity(const std::string& title, const std::string& artist, const std::string& album,
                     const std::string& imageUrl, int64_t startUnixMs, int64_t endUnixMs);
    void clear();

    struct Stats {
        bool connected = false;
        int connectAttempts = 0;    // pipe connection attempts (each tries pipes 0..9)
        int framesSent = 0;         // SET_ACTIVITY commands written
    };
    Stats stats() const;
    // Tests: waits until the worker has handled the latest state (or given up for now). False on timeout.
    bool waitIdle(int timeoutMs) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace st::app
