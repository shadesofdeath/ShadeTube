// accounts_test: offline checks of several Spotify accounts on one PC (spotify/Accounts, spotify/Session), in a
// per-process temp profile folder:
//   1. a 0.10 single-account profile migrates in place: spotify.dat, webview2\ and the per-account files move into the
//      first account's folder, shared files stay, a second load changes nothing;
//   2. a MusicBrainz-only profile: no account, data in the root; the first login adopts the root files, later ones don't;
//   3. leftovers: an interrupted login folder goes, a signed-out account keeps its folder but loses its login profile;
//   4. a damaged accounts.json is kept aside and the list is rebuilt from the folders (nothing deleted);
//   5. ids that would leave accounts\ are ignored, the per-account name matching, the read-only cookie lookup;
//   6. the Session with a fake connect (no network): add a second account without signing the first out, switch with
//      the leave / enter hooks around the data folder change, a re-login of a known user re-uses its account, a failed
//      switch keeps the current account, signing out the active account falls back to the other one, then to logged out.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/Dispatcher.h"
#include "core/ThreadPool.h"
#include "spotify/Accounts.h"
#include "spotify/Session.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace st;
using namespace st::spotify;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                                                                \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);                                          \
            ++g_failures;                                                                                          \
        }                                                                                                          \
    } while (0)

static fs::path g_base;

static fs::path freshRoot(const char* name) {
    fs::path p = g_base / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}

static void put(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary) << text;
}

static std::string read(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool has(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

// The same list as app/Accounts.cpp's perAccountFiles() (that file needs the UI, so it is not compiled here).
static const std::vector<std::wstring> kPerAccount = {L"listening.json",        L"listening-imported.json",
                                                      L"listening-lastfm.json", L"listening-listenbrainz.json",
                                                      L"blacklist.json",        L"new-releases.json",
                                                      L"smart-lists.json",      L"library-recents.json"};

static void testMigration() {
    std::printf("migration of a single-account profile:\n");
    const fs::path root = freshRoot("legacy");
    CHECK(AccountStore::writeProtected(root / L"spotify.dat", "sp_dc-legacy"));
    put(root / L"webview2" / L"EBWebView" / L"Local State", "{}");
    put(root / L"listening.json", "{\"v\":1}");
    put(root / L"listening.json.old", "old");
    put(root / L"listening-imported.json", "imported");
    put(root / L"listening-lastfm.json", "lastfm");          // imported histories with their cursors (3.42)
    put(root / L"listening-lastfm.json.old", "lastfm old");
    put(root / L"listening-listenbrainz.json", "listenbrainz");
    put(root / L"blacklist.json", "{\"version\":1}");
    put(root / L"new-releases.json", "{}");
    put(root / L"library-recents.json", "{}");
    put(root / L"settings.json", "{\"theme\":\"dark\"}");
    put(root / L"library.json", "{}");
    put(root / L"downloads.json", "{}");
    put(root / L"listening.jsonx", "not ours");

    AccountStore s(root);
    s.setPerAccountFiles(kPerAccount);
    s.load();
    CHECK(s.accounts().size() == 1);
    CHECK(!s.activeId().empty());
    const std::string id = s.activeId();
    const fs::path d = s.dir(id);
    CHECK(s.dataDir() == d);
    CHECK(s.signedIn(id));
    CHECK(s.loadCookie(id) == "sp_dc-legacy");
    CHECK(!has(root / L"spotify.dat"));
    CHECK(has(d / L"webview2" / L"EBWebView" / L"Local State"));
    CHECK(!has(root / L"webview2"));
    for (const wchar_t* f : {L"listening.json", L"listening.json.old", L"listening-imported.json", L"listening-lastfm.json",
                             L"listening-lastfm.json.old", L"listening-listenbrainz.json", L"blacklist.json",
                             L"new-releases.json", L"library-recents.json"}) {
        CHECK(has(d / f));
        CHECK(!has(root / f));
    }
    CHECK(read(d / L"listening.json") == "{\"v\":1}");
    // Per PC: stays in the root.
    for (const wchar_t* f : {L"settings.json", L"library.json", L"downloads.json", L"listening.jsonx"}) {
        CHECK(has(root / f));
        CHECK(!has(d / f));
    }
    CHECK(has(root / L"accounts.json"));
    CHECK(AccountStore::activeCookie(root) == "sp_dc-legacy");

    // A second start: the same account, nothing moves again.
    AccountStore again(root);
    again.setPerAccountFiles(kPerAccount);
    again.load();
    CHECK(again.accounts().size() == 1);
    CHECK(again.activeId() == id);
    CHECK(again.loadCookie(id) == "sp_dc-legacy");
    CHECK(has(d / L"listening.json"));
}

static void testFreshProfile() {
    std::printf("MusicBrainz-only profile, first and second login:\n");
    const fs::path root = freshRoot("fresh");
    put(root / L"listening.json", "mine");
    put(root / L"blacklist.json", "blocks");
    AccountStore s(root);
    s.setPerAccountFiles(kPerAccount);
    s.load();
    CHECK(s.accounts().empty());
    CHECK(s.activeId().empty());
    CHECK(s.dataDir() == root);
    CHECK(has(root / L"listening.json"));   // no account: the root keeps them

    const std::string a = s.newLoginFolder();
    CHECK(a.size() == 8);
    CHECK(has(s.dir(a)));
    CHECK(!s.find(a));   // not an account until add()
    Account acc;
    acc.id = a;
    s.add(acc, 1000);
    s.updateProfile(a, "spotify:user:alice", "alice", "Alice", "https://i/a.jpg");
    s.setActive(a, 1000);
    CHECK(s.saveCookie(a, "c-alice"));
    CHECK(s.save());
    CHECK(has(s.dir(a) / L"listening.json"));   // the first account adopted the profile's files
    CHECK(read(s.dir(a) / L"blacklist.json") == "blocks");
    CHECK(!has(root / L"listening.json"));

    put(root / L"listening.json", "stray");   // written later by someone: the second account must not take it
    const std::string b = s.newLoginFolder();
    CHECK(b != a);
    Account bob;
    bob.id = b;
    s.add(bob, 2000);
    CHECK(!has(s.dir(b) / L"listening.json"));
    CHECK(has(root / L"listening.json"));
    s.updateProfile(b, "spotify:user:bob", "bob", "", "");
    CHECK(s.find(b)->label() == "bob");
    CHECK(s.find(a)->label() == "Alice");
    CHECK(s.findByUser("spotify:user:bob")->id == b);
    CHECK(!s.findByUser(""));
    CHECK(s.signedIn().size() == 1);   // bob has no cookie yet
    s.setActive(b, 2000);
    s.saveCookie(b, "c-bob");
    s.save();

    AccountStore r(root);
    r.setPerAccountFiles(kPerAccount);
    r.load();
    CHECK(r.accounts().size() == 2);
    CHECK(r.activeId() == b);
    CHECK(r.find(a) && r.find(a)->name == "Alice" && r.find(a)->image == "https://i/a.jpg" && r.find(a)->addedAt == 1000);
    CHECK(r.accounts().front().id == a);   // the order they were added
    CHECK(r.signedIn().size() == 2);
    CHECK(AccountStore::activeCookie(root) == "c-bob");
}

static void testLeftovers() {
    std::printf("leftovers and sign out:\n");
    const fs::path root = freshRoot("leftovers");
    AccountStore s(root);
    s.load();
    const std::string a = s.newLoginFolder();
    Account acc;
    acc.id = a;
    s.add(acc, 10);
    s.setActive(a, 10);
    s.saveCookie(a, "c-a");
    put(s.webViewDir(a) / L"Cookies", "x");
    put(s.dir(a) / L"listening.json", "plays");
    s.save();
    const std::string interrupted = s.newLoginFolder();   // a login that never finished (crash)
    put(s.webViewDir(interrupted) / L"Cookies", "y");

    s.signOut(a);
    CHECK(!s.signedIn(a));
    CHECK(s.loadCookie(a).empty());
    CHECK(!has(s.webViewDir(a)));
    CHECK(has(s.dir(a) / L"listening.json"));   // the data stays for a later sign in

    AccountStore r(root);
    r.load();
    CHECK(!has(r.dir(interrupted)));   // not in the list: removed
    CHECK(r.find(a));                  // signed out, still known
    CHECK(r.activeId() == a);          // its data stays in use (MusicBrainz mode)
    CHECK(r.dataDir() == r.dir(a));
    CHECK(r.signedIn().empty());

    // The active record points at a folder that is gone: dropped; the other account becomes active.
    const std::string b = r.newLoginFolder();
    Account bacc;
    bacc.id = b;
    r.add(bacc, 20);
    r.saveCookie(b, "c-b");
    r.setActive(a, 30);
    r.save();
    std::error_code ec;
    fs::remove_all(r.dir(a), ec);
    AccountStore t(root);
    t.load();
    CHECK(!t.find(a));
    CHECK(t.activeId() == b);
}

static void testDamaged() {
    std::printf("damaged accounts.json:\n");
    const fs::path root = freshRoot("damaged");
    AccountStore s(root);
    s.load();
    const std::string a = s.newLoginFolder();
    Account acc;
    acc.id = a;
    s.add(acc, 10);
    s.setActive(a, 10);
    s.saveCookie(a, "c-a");
    s.save();
    const std::string loginOnly = s.newLoginFolder();
    put(s.webViewDir(loginOnly) / L"Cookies", "y");
    put(root / L"accounts.json", "{ not json");
    AccountStore r(root);
    r.load();
    CHECK(has(root / L"accounts.json.bad"));
    CHECK(r.accounts().size() == 1);
    CHECK(r.activeId() == a);
    CHECK(r.loadCookie(a) == "c-a");
    CHECK(has(r.dir(loginOnly)));   // never deleted while the list was rebuilt
    // The rebuilt list was saved: the next start reads it and only then cleans up.
    AccountStore t(root);
    t.load();
    CHECK(t.activeId() == a);
    CHECK(!has(t.dir(loginOnly)));
    // The list deleted altogether: rebuilt from the folders and written again.
    std::error_code ec;
    fs::remove(root / L"accounts.json", ec);
    AccountStore u(root);
    u.load();
    CHECK(u.activeId() == a && u.loadCookie(a) == "c-a");
    CHECK(has(root / L"accounts.json"));
}

static void testIdsAndNames() {
    std::printf("ids and names:\n");
    const fs::path root = freshRoot("ids");
    put(root / L"accounts" / L"0000000a" / L"spotify.dat", "x");
    put(root / L"outside" / L"spotify.dat", "x");
    nlohmann::json j = {{"version", 1},
                        {"active", "../outside"},
                        {"accounts", {{{"id", "../outside"}}, {{"id", "0000000a"}, {"name", "A"}}, {{"id", "0000000A"}}}}};
    put(root / L"accounts.json", j.dump());
    AccountStore s(root);
    s.setPerAccountFiles(kPerAccount);
    s.load();
    CHECK(s.accounts().size() == 1);
    CHECK(s.activeId() == "0000000a");
    CHECK(has(root / L"outside" / L"spotify.dat"));
    CHECK(s.isPerAccountFile(L"listening.json"));
    CHECK(s.isPerAccountFile(L"LISTENING.JSON"));
    CHECK(s.isPerAccountFile(L"listening.json.old"));
    CHECK(s.isPerAccountFile(L"listening-imported.json.bak"));
    CHECK(!s.isPerAccountFile(L"listening.jsonx"));
    CHECK(!s.isPerAccountFile(L"listening.json."));
    CHECK(!s.isPerAccountFile(L"settings.json"));
    CHECK(AccountStore::activeCookie(freshRoot("none")).empty());
}

// ---- Session --------------------------------------------------------------------------------------------------------

static bool pumpUntil(const std::function<bool()>& done, int ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(2);
    }
    return true;
}

static Session::Connected fakeConnect(const std::string& spDc) {
    if (spDc.rfind("bad", 0) == 0) throw std::runtime_error("anonymous token (sp_dc rejected)");
    Session::Connected c;
    c.token.accessToken = "token-" + spDc;
    c.token.expiresAtMs = 0;   // never expires: no refresh in the test
    const std::string who = spDc.substr(spDc.find('-') + 1);   // "c-alice" / "c2-alice" -> alice
    c.profile.id = "spotify:user:" + who;
    c.profile.username = who;
    c.profile.name = who == "alice" ? "Alice" : who == "bob" ? "Bob" : "";
    return c;
}

static void testSession() {
    std::printf("session: add, switch, re-login, sign out:\n");
    const fs::path root = freshRoot("session");
    put(root / L"listening.json", "alice's plays");
    Session s(root);
    s.connector = fakeConnect;
    std::vector<std::string> events;
    s.onAccountLeaving = [&] { events.push_back("leave:" + s.dataDir().filename().string()); };
    s.onAccountEntered = [&] { events.push_back("enter:" + s.dataDir().filename().string()); };
    s.loadAccounts(kPerAccount);
    s.restore();
    CHECK(s.state() == SessionState::LoggedOut);
    CHECK(s.dataDir() == root);

    // First login: a new account that takes over the profile's files; the hooks frame the folder change.
    auto slot = s.beginLogin();
    CHECK(!slot.id.empty());
    CHECK(slot.webViewDir == root / L"accounts" / std::wstring(slot.id.begin(), slot.id.end()) / L"webview2");
    int ok = -1;
    s.finishLogin(slot, "c-alice", [&](bool r) { ok = r; });
    CHECK(s.state() == SessionState::Connecting);
    CHECK(pumpUntil([&] { return ok >= 0; }));
    CHECK(ok == 1);
    CHECK(s.loggedIn());
    const std::string alice = s.activeAccountId();
    CHECK(alice == slot.id);
    CHECK(s.profile().name == "Alice");
    CHECK(s.api().hasCredentials());
    CHECK(s.api().username() == "alice");
    CHECK(events.size() == 2 && events[0] == "leave:session" && events[1] == "enter:" + alice);
    CHECK(read(s.dataDir() / L"listening.json") == "alice's plays");

    // Add Bob: Alice stays signed in, Bob becomes active, a data folder of his own.
    events.clear();
    slot = s.beginLogin();
    ok = -1;
    s.finishLogin(slot, "c-bob", [&](bool r) { ok = r; });
    CHECK(s.loggedIn());   // Alice keeps the session while Bob connects
    CHECK(s.activeAccountId() == alice);
    CHECK(pumpUntil([&] { return ok >= 0; }));
    const std::string bob = s.activeAccountId();
    CHECK(ok == 1 && bob != alice);
    CHECK(s.profile().name == "Bob");
    CHECK(s.signedInAccounts().size() == 2);
    CHECK(s.accounts().loadCookie(alice) == "c-alice");
    CHECK(events.size() == 2 && events[0] == "leave:" + alice && events[1] == "enter:" + bob);
    CHECK(!has(s.dataDir() / L"listening.json"));

    // Quick switch back to Alice: no login, the hooks around it, her folder again.
    events.clear();
    ok = -1;
    s.switchTo(alice, [&](bool r) { ok = r; });
    CHECK(s.switchingTo() == alice);
    CHECK(s.activeAccountId() == bob);   // until her token is in
    CHECK(pumpUntil([&] { return ok >= 0; }));
    CHECK(ok == 1);
    CHECK(s.switchingTo().empty());
    CHECK(s.activeAccountId() == alice && s.profile().name == "Alice");
    CHECK(events.size() == 2 && events[0] == "leave:" + bob && events[1] == "enter:" + alice);
    // Switching to the active account is a no-op.
    events.clear();
    ok = -1;
    s.switchTo(alice, [&](bool r) { ok = r; });
    CHECK(ok == 1 && events.empty());

    // Alice signs in again (a fresh login of the same Spotify user): her account, not a third one.
    slot = s.beginLogin();
    ok = -1;
    s.finishLogin(slot, "c2-alice", [&](bool r) { ok = r; });
    CHECK(pumpUntil([&] { return ok >= 0; }));
    CHECK(ok == 1);
    CHECK(s.accounts().accounts().size() == 2);
    CHECK(s.activeAccountId() == alice);
    CHECK(s.accounts().loadCookie(alice) == "c2-alice");
    CHECK(!has(s.accounts().dir(slot.id)));   // that login's folder went
    CHECK(events.empty());                      // same account: no data swap

    // A closed login window: nothing changes, its folder goes.
    slot = s.beginLogin();
    ok = -1;
    s.finishLogin(slot, "", [&](bool r) { ok = r; });
    CHECK(ok == 0 && !has(s.accounts().dir(slot.id)) && s.activeAccountId() == alice);

    // A switch that fails (Bob's cookie was revoked): Alice stays.
    s.accounts().saveCookie(bob, "bad-bob");
    events.clear();
    ok = -1;
    s.switchTo(bob, [&](bool r) { ok = r; });
    CHECK(pumpUntil([&] { return ok >= 0; }));
    CHECK(ok == 0 && s.loggedIn() && s.activeAccountId() == alice && events.empty());
    s.accounts().saveCookie(bob, "c-bob");

    // Alice signs out: Bob takes over (playback would go on), Alice's data stays on disk.
    events.clear();
    s.logout();
    CHECK(!s.accounts().signedIn(alice));
    CHECK(pumpUntil([&] { return s.loggedIn(); }));
    CHECK(s.activeAccountId() == bob);
    CHECK(events.size() == 2 && events[0] == "leave:" + alice && events[1] == "enter:" + bob);
    CHECK(has(s.accounts().dir(alice) / L"listening.json"));

    // Bob signs out too: logged out, his data stays active (MusicBrainz mode keeps his history on screen).
    events.clear();
    s.logout();
    CHECK(s.state() == SessionState::LoggedOut);
    CHECK(s.activeAccountId() == bob);
    CHECK(s.signedInAccounts().empty());
    CHECK(events.empty());
    CHECK(!s.api().hasCredentials());

    // Alice signs in again later: her old folder (and history) comes back.
    slot = s.beginLogin();
    ok = -1;
    s.finishLogin(slot, "c3-alice", [&](bool r) { ok = r; });
    CHECK(pumpUntil([&] { return ok >= 0; }));
    CHECK(s.activeAccountId() == alice);
    CHECK(read(s.dataDir() / L"listening.json") == "alice's plays");

    // The next start restores the active account.
    Session next(root);
    next.connector = fakeConnect;
    next.loadAccounts(kPerAccount);
    CHECK(next.activeAccountId() == alice);
    next.restore();
    CHECK(next.state() == SessionState::Connecting);
    CHECK(pumpUntil([&] { return next.loggedIn(); }));
    CHECK(next.profile().name == "Alice");

    // The active account has no cookie any more but another one is signed in: the next start takes that one.
    {
        AccountStore st(root);
        st.load();
        st.signOut(alice);
        st.saveCookie(bob, "c-bob");
        st.save();
    }
    Session third(root);
    third.connector = fakeConnect;
    int entered = 0;
    third.onAccountEntered = [&] { ++entered; };
    third.loadAccounts(kPerAccount);
    CHECK(third.activeAccountId() == alice);
    third.restore();
    CHECK(third.state() == SessionState::Connecting);
    CHECK(pumpUntil([&] { return third.loggedIn(); }));
    CHECK(third.activeAccountId() == bob && third.profile().name == "Bob" && entered == 1);
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    g_base = fs::path(tmp) / (L"shadetube-accounts-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(g_base, ec);
    ThreadPool pool(4);
    ThreadPool::setShared(&pool);
    Dispatcher::init();

    testMigration();
    testFreshProfile();
    testLeftovers();
    testDamaged();
    testIdsAndNames();
    testSession();

    Dispatcher::shutdown();
    ThreadPool::setShared(nullptr);
    fs::remove_all(g_base, ec);
    if (g_failures) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("all accounts checks passed\n");
    return 0;
}
