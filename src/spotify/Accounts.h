#pragma once
// Several Spotify accounts on one PC, switched without logging in again. Every account has a folder of its own,
// <data>\accounts\<id>\ (id: 8 hex digits, made up here): its sp_dc cookie (spotify.dat, DPAPI-encrypted), the WebView2
// profile of the login that produced it (webview2\: a login never sees another account's cookies) and the app's
// per-account files (listening history, blocklist, new releases... the names come from the app). The list itself is
// <data>\accounts.json: id, the Spotify user once known, the profile shown in the switcher, the active account.
//
// An account is signed in while its cookie file exists. Signing out deletes the cookie and the login profile; the folder
// and the record stay, so a later sign in to the same Spotify user finds its history again (re-associated by user id).
// With no account at all (MusicBrainz only) the per-account files live in <data> itself; the first account adopts them.
//
// Standalone (core only + DPAPI) so tests drive it with a temp folder. UI thread only; every call is a few small file
// operations.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace st::spotify {

struct Account {
    std::string id;         // folder name under accounts\ (8 hex digits)
    std::string user;       // spotify:user:<name>, "" until the account first connected
    std::string username;   // the user name (playlist writes need it), shown when there is no display name
    std::string name;       // display name
    std::string image;      // avatar URL ("" = none)
    int64_t addedAt = 0;    // unix seconds
    int64_t usedAt = 0;     // unix seconds it was last the active account
    // The display name / the user name / the id.
    std::string label() const { return !name.empty() ? name : !username.empty() ? username : id; }
};

class AccountStore {
public:
    explicit AccountStore(std::filesystem::path root);

    // The app's files that belong to an account (e.g. L"listening.json"): a root file whose name is one of these, or one
    // of these followed by "." and anything (".old", ".bak", ".tmp"...), moves into the first account's folder.
    void setPerAccountFiles(std::vector<std::wstring> names) { perAccount_ = std::move(names); }

    // Reads accounts.json. A profile from before multi-account support (spotify.dat in the root, no accounts.json)
    // becomes the first account: its cookie, the login's WebView2 folder and the per-account files move into its folder.
    // An unreadable accounts.json is kept as accounts.json.bad and the list is rebuilt from the folders. Leftovers are
    // removed: folders of interrupted logins (not in the list) and the login profile of signed-out accounts.
    void load();
    bool save() const;   // write-then-rename; false on failure (logged)

    const std::filesystem::path& root() const { return root_; }
    const std::vector<Account>& accounts() const { return accounts_; }   // in the order they were added
    const Account* find(const std::string& id) const;
    const Account* findByUser(const std::string& user) const;   // "" never matches
    std::vector<Account> signedIn() const;                       // accounts with a cookie, in order
    bool signedIn(const std::string& id) const;

    // The account whose data is in use: the one signed in to, or the last one signed out of (its history stays on
    // screen in MusicBrainz mode). "" with no accounts.
    const std::string& activeId() const { return active_; }
    const Account* active() const { return find(active_); }
    void setActive(const std::string& id, int64_t now);          // also its usedAt

    std::filesystem::path dir(const std::string& id) const;      // root\accounts\<id>
    std::filesystem::path webViewDir(const std::string& id) const;
    // Where the per-account files of the active account live: its folder, or the root without accounts.
    std::filesystem::path dataDir() const;

    // A new folder for a login (not in the list yet): its id. Becomes an account with add(), else discard() it.
    std::string newLoginFolder();
    // Adds the account of a finished login (`id` from newLoginFolder). The very first account adopts the root's
    // per-account files. Not active yet.
    void add(Account a, int64_t now);
    // Deletes a login folder that did not become an account (best effort: WebView2 may still hold it for a moment;
    // load() removes what is left at the next start).
    bool discard(const std::string& id);   // true when the folder is gone
    // The profile seen at a connect (user id, names, avatar).
    void updateProfile(const std::string& id, const std::string& user, const std::string& username, const std::string& name,
                       const std::string& image);

    // sp_dc of an account (DPAPI). "" when missing / unreadable.
    bool saveCookie(const std::string& id, const std::string& spDc) const;
    std::string loadCookie(const std::string& id) const;
    // Signs an account out: its cookie and its login profile go (best effort for the latter), the folder and record stay.
    void signOut(const std::string& id) const;

    // DPAPI helpers (also used for the pre-0.11 root cookie).
    static bool writeProtected(const std::filesystem::path& file, const std::string& secret);
    static std::string readProtected(const std::filesystem::path& file);
    // Read-only (live tests): the active account's cookie under `root`, else a pre-0.11 root spotify.dat. Never
    // migrates or cleans up anything.
    static std::string activeCookie(const std::filesystem::path& root);

    // Whether `name` is (a sibling of) one of the per-account files (exposed for tests).
    bool isPerAccountFile(const std::wstring& name) const;

private:
    void migrateLegacy(int64_t now);
    void recoverFromFolders();
    void cleanup() const;
    void adoptRootFiles(const std::string& id) const;
    Account* findMutable(const std::string& id);

    std::filesystem::path root_;
    std::vector<std::wstring> perAccount_;
    std::vector<Account> accounts_;
    std::string active_;
    bool listTrusted_ = true;   // false after a broken accounts.json: never delete folders then
};

} // namespace st::spotify
