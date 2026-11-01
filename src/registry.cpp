#include "registry.h"
#include "common.h"
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

std::mutex g_eventMtx;
struct MenuEventEntry {
    int id;
    PBZFM_MenuEventFn fn;
    void* ud;
};
std::vector<MenuEventEntry> g_events;
int g_nextEventId = 1;

// sidebar hit rects for automation (written by the render thread; ROOT rows)
float g_sectionRects[32][4];
int g_sectionRectCount = 0;

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

void RegistryResetSectionRects() { g_sectionRectCount = 0; }

void RegistryRecordSectionRect(int index, const float xyxy[4]) {
    if (index < 0 || index >= 32) return;
    for (int i = 0; i < 4; ++i) g_sectionRects[index][i] = xyxy[i];
    if (index + 1 > g_sectionRectCount) g_sectionRectCount = index + 1;
}

int RegistryGetSectionRect(int index, float out_xyxy[4]) {
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
            g_hotkeys.erase(it);
            return true;
        }
    }
    return false;
}

bool RegistryDispatchHotkey(unsigned vk, bool up, unsigned mods) {
    // down-edge bookkeeping keys on (mods, vk): two chords sharing the final
    // key but not the modifiers repeat independently.
    const unsigned kkey = (mods << 16) | (vk & 0xFFFFu);
    static std::set<unsigned> down;   // hook thread only
    std::vector<HotkeyEntry> fired;
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMtx);
        for (const auto& h : g_hotkeys) {
            if (h.vk == vk && h.mods == mods) fired.push_back(h);
        }
    }
    if (fired.empty()) return false;
    if (up) {
        down.erase(kkey);
        return true;
    }
    if (down.count(kkey)) return true;  // held: swallow repeats, fire once per press
    down.insert(kkey);
    for (const auto& h : fired) h.fn(h.ud);
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
    std::vector<MenuEventEntry> fired;
    {
        std::lock_guard<std::mutex> lock(g_eventMtx);
        fired = g_events;
    }
    for (const auto& e : fired) e.fn(open ? 1 : 0, e.ud);
}

} // namespace pbzfm
