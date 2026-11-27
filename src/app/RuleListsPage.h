#pragma once
// Kurallı listeler in the app (app/RuleLists is the pure part): the lists the user made (rule-lists.json, per account),
// the engine that keeps them evaluated, the list page, the editor, and the hooks the sidebar / Library / Home use.
//
// Engine: one rules::Pool, owned by a chain of jobs on a Low priority worker (one job at a time; the UI thread never
// touches it). Every change of an input (the local likes / playlists, the Spotify collections cached by app/RuleSources,
// the local library, the downloads, the listening history, the liked / downloaded ids, BPMs, the day) posts a job with
// only what changed (fingerprints on the UI side: nothing unchanged is copied or folded again); the job applies it to the
// pool and evaluates again only the lists whose dependencies moved (rules::depsOf + the pool's revisions). Results: a
// summary per list (song count, length, four covers: persisted, so the sidebar shows them at once after a start) and
// the songs of the lists a page shows. The listening history is aggregated on the UI thread at most every 15 s. The
// pool exists only while a list exists or the editor is open (the last list deleted, it is released).
//
// Routes: Route{RouteKind::SmartList, "rule:<id>"} (the list; "#play" appended starts it), "rule-edit:<id>" (its
// editor) and "rule-new" (a new list's editor).
//
// UI thread only.
#include "app/RuleLists.h"
#include "core/Async.h"
#include "gfx/Types.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace st::gfx {
class Canvas;
}
namespace st::ui {
struct MenuItem;
}

namespace st::app {

class Page;

namespace rulelists {

struct List {
    std::string id;                     // "r<8 hex>"
    rules::Definition def;
    int64_t created = 0, updated = 0;   // unix seconds
    // The last evaluation (kept in the file).
    size_t count = 0;
    int64_t totalMs = 0;
    std::vector<catalog::Image> covers;   // up to four distinct covers of its first songs
    bool evaluated = false;
};

void init();   // initFeatures(): loads the lists, wires the inputs
const std::vector<List>& lists();   // newest first
const List* find(const std::string& id);
// The lists or their results changed. Coalesced; dies with `owner`.
void subscribe(Lifetime::Ref owner, std::function<void()> fn);

std::wstring subtitle(const List& l);   // "Kurallı liste · 42 şarkı"
// The cover: a mosaic of the list's covers above a band with its name (as the smart lists' covers), in its tile color.
void drawCover(gfx::Canvas& c, const List& l, const gfx::Rect& r, float radius);
std::vector<ui::MenuItem> menuItems(const std::string& id);   // a list's card / row menu
void showMenu(const std::string& id, gfx::Point windowPos);
// "Yeni kurallı liste…" and "Kurallı liste içe aktar…" (the sidebar's "+" menu).
std::vector<ui::MenuItem> createMenuItems();

} // namespace rulelists

std::unique_ptr<Page> makeRuleListPage(const std::string& id);

} // namespace st::app
