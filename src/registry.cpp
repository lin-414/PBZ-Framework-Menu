#include "registry.h"
#include "common.h"
#include "schema.h"
#include <deque>
#include <set>

namespace pbzfm {

namespace {
std::recursive_mutex g_mtx;
std::vector<std::unique_ptr<Section>> g_sections;   // roots
// Current section is tracked by PATH, not pointer: DeleteSection /
// RenameSection / tree edits can run on other threads and a raw pointer would
// dangle. Resolution happens on use, under the registry mutex.
thread_local std::string tls_currentPath;

std::mutex g_hotkeyMtx;
struct HotkeyEntry {
    int id;
    unsigned vk;
    unsigned mods;   // PBZFM_MOD_* bits, exact match
    PBZFM_HotkeyFn fn;
    void* ud;
};
std::vector<HotkeyEntry> g_hotkeys;
int g_nextHotkeyId = 1;

// Review H6: hook thread -> render thread handoff. The hook only appends {fn,ud};
// the flush re-checks module liveness before calling, so a RemoveHotkey between
// the two steps is harmless. Bounded: a stuck render loop must not grow memory.
struct QueuedHotkey {
    PBZFM_HotkeyFn fn = nullptr;
    void* ud = nullptr;
};
std::deque<QueuedHotkey> g_hotkeyQueue;
constexpr size_t kMaxQueuedHotkeys = 32;
bool g_hotkeyQueueWarned = false;
// Down-edge bookkeeping shared by the hook queue path and the synchronous test
// path (two threads), so it lives under g_hotkeyMtx.
std::set<unsigned> g_hotkeyDown;

std::mutex g_eventMtx;
struct MenuEventEntry {
    int id;
    PBZFM_MenuEventFn fn;
    void* ud;
};
std::vector<MenuEventEntry> g_events;
int g_nextEventId = 1;

// sidebar hit rects for automation (written by the render thread; ROOT rows)
// L4: the probe thread reads these while the render thread writes them, so the
// count+data update happens under one mutex (a consistent snapshot, no torn
// rect); over-capacity roots are logged instead of silently disappearing.
constexpr int kSectionRectSlots = 32;
float g_sectionRects[kSectionRectSlots][4];
int g_sectionRectCount = 0;
std::mutex g_sectionRectMtx;
std::atomic<bool> g_sectionRectOverflowLogged{ false };

// ---- callback ownership (review H5) ----
void FrameworkModuleAnchor() {}

// Module the framework itself lives in; distinguishes its own chrome callbacks
// from client mod callbacks during a bulk clear.
HMODULE FrameworkModule() {
    static HMODULE m = [] {
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)&FrameworkModuleAnchor, &h);
        return h;
    }();
    return m;
}

// Module that owns an entry point, or null when the address no longer belongs
// to any loaded module (the mod DLL hot-unloaded). Safe on a dead function
// pointer: the loader API never dereferences the address.
HMODULE CallbackOwner(uintptr_t fn) {
    HMODULE m = nullptr;
    if (!fn) return nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)fn, &m)) {
        return nullptr;
    }
    return m;
}

bool CallbackModuleAlive(uintptr_t fn) { return CallbackOwner(fn) != nullptr; }

// ConfigDefs whose change callback api.cpp handed us, so the sweep can clear a
// changeFn owned by an unloaded module (review H5). ConfigDef lifetimes live in
// schema.cpp's table and never go away during the session.
std::vector<ConfigDef*> g_configCallbacks;

// "A/B\/C" -> {"A", "B/C"}: '/' splits segments, a backslash escapes the next
// byte so a literal '/' or '\' can live inside a segment. Empty segments are
// dropped (tolerant of "A//B" and stray leading/trailing slashes).
std::vector<std::string> SplitPath(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        if (c == '\\' && i + 1 < path.size()) { cur += path[++i]; continue; }
        if (c == '/') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string EscapeSegment(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '/' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

Section* FindChild(Section* parent, const std::string& name) {
    if (parent) {
        for (auto& c : parent->children) {
            if (c->name == name) return c.get();
        }
        return nullptr;
    }
    for (auto& r : g_sections) {
        if (r->name == name) return r.get();
    }
    return nullptr;
}

Section* CreateChild(Section* parent, const std::string& name) {
    auto s = std::make_unique<Section>();
    s->name = name;
    s->parent = parent;
    Section* raw = s.get();
    if (parent) parent->children.push_back(std::move(s));
    else g_sections.push_back(std::move(s));
    return raw;
}

Section* FindPathLocked(const std::string& path) {
    Section* cur = nullptr;
    for (const std::string& seg : SplitPath(path)) {
        cur = FindChild(cur, seg);
        if (!cur) return nullptr;
    }
    return cur;
}

} // namespace

void RegistryResetSectionRects() {
    std::lock_guard<std::mutex> lock(g_sectionRectMtx);
    g_sectionRectCount = 0;
}

void RegistryRecordSectionRect(int index, const float xyxy[4]) {
    if (index < 0 || index >= kSectionRectSlots) {
        // L4: say it once - an over-capacity root is not reported, and silence
        // would look like a missing render rather than a table limit.
        if (!g_sectionRectOverflowLogged.exchange(true, std::memory_order_relaxed))
            Log("[registry] WARNING: section rect index %d exceeds the %d-slot probe table - not reported",
                index, kSectionRectSlots);
        return;
    }
    std::lock_guard<std::mutex> lock(g_sectionRectMtx);
    for (int i = 0; i < 4; ++i) g_sectionRects[index][i] = xyxy[i];
    if (index + 1 > g_sectionRectCount) g_sectionRectCount = index + 1;
}

int RegistryGetSectionRect(int index, float out_xyxy[4]) {
    std::lock_guard<std::mutex> lock(g_sectionRectMtx);
    if (index < 0 || index >= g_sectionRectCount) return 0;
    for (int i = 0; i < 4; ++i) out_xyxy[i] = g_sectionRects[index][i];
    return 1;
}

std::recursive_mutex& RegistryMutex() { return g_mtx; }
std::vector<std::unique_ptr<Section>>& RegistrySections() { return g_sections; }

Section* RegistryGetOrCreatePath(const char* path_utf8) {
    std::vector<std::string> segs = SplitPath(path_utf8 ? path_utf8 : "");
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    Section* cur = nullptr;
    std::string built;
    for (const std::string& seg : segs) {
        Section* next = FindChild(cur, seg);
        if (!next) {
            next = CreateChild(cur, seg);
            Log("[api] section node created: %s%s%s", built.c_str(), built.empty() ? "" : "/",
                EscapeSegment(seg).c_str());
        }
        built = built.empty() ? EscapeSegment(seg) : built + "/" + EscapeSegment(seg);
        cur = next;
    }
    if (!cur) return nullptr;
    tls_currentPath = RegistryPathOf(cur);
    return cur;
}

Section* RegistryFindPath(const char* path_utf8) {
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    return FindPathLocked(path_utf8 ? path_utf8 : "");
}

std::string RegistryPathOf(const Section* node) {
    if (!node) return {};
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    std::vector<std::string> segs;
    for (const Section* n = node; n; n = n->parent) segs.push_back(EscapeSegment(n->name));
    std::string out;
    for (auto it = segs.rbegin(); it != segs.rend(); ++it) {
        if (!out.empty()) out += '/';
        out += *it;
    }
    return out;
}

Section* RegistryGetOrCreateSection(const char* name_utf8) {
    // v1 entry point: a single-segment path, i.e. a root - exactly the old
    // "root sections are unique by name" behavior.
    return RegistryGetOrCreatePath(name_utf8);
}

Section* RegistryCurrentSection() {
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    if (tls_currentPath.empty()) return nullptr;
    return FindPathLocked(tls_currentPath);
}

void RegistryAddPage(Section* section, const Page& page) {
    if (!section) return;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    section->pages.push_back(page);
    Log("[api] page registered: %s / %s%s", RegistryPathOf(section).c_str(), page.label.c_str(),
        page.schemaCfg ? " (schema)" : " (custom)");
}

bool RegistryRenameSection(const char* target_path, const char* new_name) {
    if (!target_path || !new_name || !*new_name) return false;
    if (strchr(new_name, '/')) return false;   // a name is a single segment
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    Section* node = FindPathLocked(target_path);
    if (!node) return false;
    Section* sib = node->parent;   // null sibling list = roots
    auto clash = [&](const std::unique_ptr<Section>& s) {
        return s.get() != node && s->name == new_name;
    };
    if (sib) {
        for (auto& c : sib->children) if (clash(c)) return false;
    } else {
        for (auto& r : g_sections) if (clash(r)) return false;
    }
    Log("[api] section renamed: %s -> %s", RegistryPathOf(node).c_str(), new_name);
    node->name = new_name;
    return true;
}

bool RegistryDeleteSection(const char* target_path) {
    if (!target_path) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    Section* node = FindPathLocked(target_path);
    if (!node) return false;
    Log("[api] section deleted: %s (pages=%zu children=%zu)", RegistryPathOf(node).c_str(),
        node->pages.size(), node->children.size());
    if (node->parent) {
        auto& v = node->parent->children;
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it->get() == node) { v.erase(it); return true; }
        }
        return false;
    }
    for (auto it = g_sections.begin(); it != g_sections.end(); ++it) {
        if (it->get() == node) { g_sections.erase(it); return true; }
    }
    return false;
}

bool RegistryRemovePage(const char* target_path, const char* label_utf8) {
    if (!target_path || !label_utf8) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    Section* node = FindPathLocked(target_path);
    if (!node) return false;
    for (auto p = node->pages.begin(); p != node->pages.end(); ++p) {
        if (p->label == label_utf8) {
            Log("[api] page removed: %s / %s", RegistryPathOf(node).c_str(), label_utf8);
            node->pages.erase(p);
            return true;
        }
    }
    return false;
}

// Review H5: `frameworkOnly` selects the bulk-clear behavior (drop everything
// the framework DLL does not own); otherwise only orphaned entry points are
// dropped.
void PurgeNodesLocked(Section& node, bool frameworkOnly, HMODULE self, int* cleared) {
    for (Page& p : node.pages) {
        if (!p.fn) continue;
        const HMODULE owner = CallbackOwner((uintptr_t)p.fn);
        const bool drop = frameworkOnly ? (owner != self) : (owner == nullptr);
        if (!drop) continue;
        // The row stays (label/schema intact) but renders as an empty page: menu
        // checks the fn for null before calling it (review H5).
        p.fn = nullptr;
        p.ud = nullptr;
        ++*cleared;
    }
    for (auto& c : node.children) PurgeNodesLocked(*c, frameworkOnly, self, cleared);
}

void RegistryPurgeOrphanedCallbacks() {
    const HMODULE self = FrameworkModule();
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    int pages = 0;
    for (auto& r : g_sections) PurgeNodesLocked(*r, false, self, &pages);

    int changes = 0;
    for (ConfigDef* cfg : g_configCallbacks) {
        if (cfg->changeFn && !CallbackModuleAlive((uintptr_t)cfg->changeFn)) {
            cfg->changeFn = nullptr;
            cfg->changeUd = nullptr;
            ++changes;
        }
    }

    int hotkeys = 0;
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMtx);
        for (auto it = g_hotkeys.begin(); it != g_hotkeys.end();) {
            if (!CallbackModuleAlive((uintptr_t)it->fn)) {
                Log("[api] hotkey dropped (owner module unloaded): id=%d", it->id);
                // L5: the pressed state belongs to a chord that no longer
                // exists; re-registering the same chord must fire on the next
                // press instead of being swallowed by the stale down bit.
                g_hotkeyDown.erase((it->mods << 16) | (it->vk & 0xFFFFu));
                it = g_hotkeys.erase(it);
                ++hotkeys;
            } else {
                ++it;
            }
        }
    }
    int events = 0;
    {
        std::lock_guard<std::mutex> lock(g_eventMtx);
        for (auto it = g_events.begin(); it != g_events.end();) {
            if (!CallbackModuleAlive((uintptr_t)it->fn)) {
                it = g_events.erase(it);
                ++events;
            } else {
                ++it;
            }
        }
    }
    if (pages || changes || hotkeys || events) {
        Log("[api] purge: dropped %d page, %d change, %d hotkey, %d menu-event callback(s) "
            "whose module unloaded", pages, changes, hotkeys, events);
    }
}

void RegistryClearModCallbacks() {
    const HMODULE self = FrameworkModule();
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    int pages = 0;
    for (auto& r : g_sections) PurgeNodesLocked(*r, true, self, &pages);

    int changes = 0;
    for (ConfigDef* cfg : g_configCallbacks) {
        if (cfg->changeFn && CallbackOwner((uintptr_t)cfg->changeFn) != self) {
            cfg->changeFn = nullptr;
            cfg->changeUd = nullptr;
            ++changes;
        }
    }
    // The framework registers no hotkeys or menu events itself, so a bulk clear
    // removes all of them (review H5).
    int hotkeys = 0, events = 0;
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMtx);
        hotkeys = (int)g_hotkeys.size();
        g_hotkeys.clear();
        // H6: drop presses still waiting for the render thread too; their
        // callbacks belong to modules that are about to be gone.
        g_hotkeyQueue.clear();
        g_hotkeyDown.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_eventMtx);
        events = (int)g_events.size();
        g_events.clear();
    }
    Log("[api] cleared mod callbacks: %d page, %d change, %d hotkey, %d menu event",
        pages, changes, hotkeys, events);
}

void RegistryTrackConfigCallback(void* config) {
    if (!config) return;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    for (ConfigDef* c : g_configCallbacks) {
        if (c == config) return;
    }
    g_configCallbacks.push_back((ConfigDef*)config);
}

int RegistryAddHotkey(unsigned vk, unsigned mods, PBZFM_HotkeyFn fn, void* ud) {
    if (!fn || !vk) return 0;
    std::lock_guard<std::mutex> lock(g_hotkeyMtx);
    int id = g_nextHotkeyId++;
    g_hotkeys.push_back({ id, vk, mods & 0xFu, fn, ud });
    Log("[api] hotkey registered: id=%d vk=0x%02X mods=0x%X", id, vk, mods & 0xFu);
    return id;
}

bool RegistryRemoveHotkey(int id) {
    if (id <= 0) return false;
    std::lock_guard<std::mutex> lock(g_hotkeyMtx);
    for (auto it = g_hotkeys.begin(); it != g_hotkeys.end(); ++it) {
        if (it->id == id) {
            Log("[api] hotkey removed: id=%d vk=0x%02X", id, it->vk);
            // L5: forget the "held" bit with the entry; otherwise a re-registered
            // chord would need a release+repress before it fires again.
            g_hotkeyDown.erase((it->mods << 16) | (it->vk & 0xFFFFu));
            g_hotkeys.erase(it);
            return true;
        }
    }
    return false;
}

namespace {
// Shared by the queue and sync flavors: caller holds g_hotkeyMtx. Prunes entries
// whose owner module unloaded while matching (vk, mods) and returns the callbacks.
std::vector<QueuedHotkey> MatchHotkeysLocked(unsigned vk, unsigned mods) {
    std::vector<QueuedHotkey> matched;
    for (auto it = g_hotkeys.begin(); it != g_hotkeys.end();) {
        if (it->vk != vk || it->mods != mods) {
            ++it;
            continue;
        }
        // Review H5: a hot-unloaded mod's entry point no longer lives in any
        // loaded module - drop the entry instead of calling into unmapped code.
        // Checked only for a matched chord, so the common "no hotkey" path stays
        // cheap and the hook thread never takes the registry lock.
        if (!CallbackModuleAlive((uintptr_t)it->fn)) {
            Log("[api] hotkey dropped (owner module unloaded): id=%d", it->id);
            it = g_hotkeys.erase(it);
            continue;
        }
        matched.push_back({ it->fn, it->ud });
        ++it;
    }
    return matched;
}
} // namespace

bool RegistryQueueHotkey(unsigned vk, bool up, unsigned mods) {
    // Review H6: called on the LL hook thread. It only records intent; the
    // callback itself runs in RegistryFlushQueuedHotkeys() on the render thread.
    // Down edges fire once per press: two chords sharing the final key but not
    // the modifiers repeat independently.
    mods &= 0xFu;
    const unsigned kkey = (mods << 16) | (vk & 0xFFFFu);
    std::lock_guard<std::mutex> lock(g_hotkeyMtx);
    const std::vector<QueuedHotkey> matched = MatchHotkeysLocked(vk, mods);
    if (matched.empty()) {
        // L5: no callback owns this chord any more - drop its down state so a
        // re-registered hotkey fires on the very next press.
        g_hotkeyDown.erase(kkey);
        return false;
    }
    if (up) {
        g_hotkeyDown.erase(kkey);
        return true;
    }
    if (g_hotkeyDown.count(kkey)) return true;   // held: swallow repeats, fire once
    g_hotkeyDown.insert(kkey);
    for (const QueuedHotkey& q : matched) {
        if (g_hotkeyQueue.size() >= kMaxQueuedHotkeys) {
            // Drop-newest: the older presses are the ones the player actually
            // expects to act on; logging once keeps a stuck render loop quiet.
            if (!g_hotkeyQueueWarned) {
                g_hotkeyQueueWarned = true;
                Log("[api] hotkey queue full (%zu) - dropping new presses until it drains",
                    kMaxQueuedHotkeys);
            }
            break;
        }
        g_hotkeyQueue.push_back(q);
    }
    return true;
}

void RegistryFlushQueuedHotkeys() {
    // Review H6: render thread, outside the ImGui frame. Callbacks may call the
    // PBZFM API (registry/config locks are free at this point) but must be quick
    // and must never block: they delay the overlay's next frame. No ImGui widget
    // calls here - a frame is not active, so widgets would assert/crash.
    std::deque<QueuedHotkey> batch;
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMtx);
        batch.swap(g_hotkeyQueue);
    }
    for (const QueuedHotkey& q : batch) {
        if (!q.fn) continue;
        if (!CallbackModuleAlive((uintptr_t)q.fn)) continue;   // unloaded since queueing
        q.fn(q.ud);
    }
}

bool RegistryDispatchHotkey(unsigned vk, bool up, unsigned mods) {
    mods &= 0xFu;
    const unsigned kkey = (mods << 16) | (vk & 0xFFFFu);
    std::vector<QueuedHotkey> fired;
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMtx);
        fired = MatchHotkeysLocked(vk, mods);
        if (fired.empty()) {
            // L5: same rule as the queue flavor - a chord with no owner must not
            // keep a held bit that would swallow the next press after re-register.
            g_hotkeyDown.erase(kkey);
            return false;
        }
        if (up) {
            g_hotkeyDown.erase(kkey);
            return true;
        }
        if (g_hotkeyDown.count(kkey)) return true;  // held: fire once per press
        g_hotkeyDown.insert(kkey);
    }
    // Review H6: the snapshot above was taken under the mutex and the calls
    // below run outside it, so a callback may safely register/unregister. This
    // synchronous flavor is for automation/tests only - the live hook uses
    // RegistryQueueHotkey() so mod code never runs inside the LL hook.
    for (const QueuedHotkey& q : fired) q.fn(q.ud);
    return true;
}

int RegistryAddMenuEvent(PBZFM_MenuEventFn fn, void* ud) {
    if (!fn) return 0;
    std::lock_guard<std::mutex> lock(g_eventMtx);
    int id = g_nextEventId++;
    g_events.push_back({ id, fn, ud });
    Log("[api] menu event registered: id=%d", id);
    return id;
}

void RegistryRemoveMenuEvent(int id) {
    if (id <= 0) return;
    std::lock_guard<std::mutex> lock(g_eventMtx);
    for (auto it = g_events.begin(); it != g_events.end(); ++it) {
        if (it->id == id) {
            g_events.erase(it);
            return;
        }
    }
}

void RegistryDispatchMenuEvent(bool open) {
    // Review H5: open/close is a rare transition and the per-frame sweep has not
    // necessarily run yet (the event fires before the first frame), so purge
    // here before snapshotting.
    RegistryPurgeOrphanedCallbacks();
    std::vector<MenuEventEntry> fired;
    {
        std::lock_guard<std::mutex> lock(g_eventMtx);
        fired = g_events;
    }
    // Review H6: snapshot under the mutex, call outside it; render thread only.
    // Callbacks must not block (they delay the overlay's frame).
    for (const auto& e : fired) {
        if (!CallbackModuleAlive((uintptr_t)e.fn)) continue;   // unloaded since the snapshot
        e.fn(open ? 1 : 0, e.ud);
    }
}

} // namespace pbzfm
