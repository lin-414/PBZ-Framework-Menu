#include "registry.h"
#include "common.h"
#include <set>

namespace pbzfm {

namespace {
std::recursive_mutex g_mtx;
std::vector<std::unique_ptr<Section>> g_sections;
// Current section is tracked by NAME, not pointer: DeleteSection/RenameSection
// can run on other threads and a raw pointer here would dangle. Resolution
// happens on use, under the registry mutex.
thread_local std::string tls_currentName;

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

// sidebar hit rects for automation (written by the render thread)
float g_sectionRects[32][4];
int g_sectionRectCount = 0;
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

Section* RegistryGetOrCreateSection(const char* name_utf8) {
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    for (auto& s : g_sections) {
        if (s->name == name_utf8) {
            tls_currentName = s->name;
            return s.get();
        }
    }
    auto s = std::make_unique<Section>();
    s->name = name_utf8 ? name_utf8 : "?";
    Section* raw = s.get();
    g_sections.push_back(std::move(s));
    tls_currentName = raw->name;
    Log("[api] section registered: %s", raw->name.c_str());
    return raw;
}

Section* RegistryCurrentSection() {
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    if (tls_currentName.empty()) return nullptr;
    for (auto& s : g_sections) {
        if (s->name == tls_currentName) return s.get();
    }
    return nullptr;   // renamed or deleted out from under this thread
}

void RegistryAddPage(Section* section, const Page& page) {
    if (!section) return;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    section->pages.push_back(page);
    Log("[api] page registered: %s / %s%s", section->name.c_str(), page.label.c_str(),
        page.schemaCfg ? " (schema)" : " (custom)");
}

bool RegistryRenameSection(const char* old_utf8, const char* new_utf8) {
    if (!old_utf8 || !new_utf8 || !*new_utf8) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    Section* target = nullptr;
    for (auto& s : g_sections) {
        if (s->name == old_utf8) { target = s.get(); break; }
    }
    if (!target) return false;
    for (auto& s : g_sections) {
        if (s.get() != target && s->name == new_utf8) return false;   // collision
    }
    Log("[api] section renamed: %s -> %s", target->name.c_str(), new_utf8);
    target->name = new_utf8;
    return true;
}

bool RegistryDeleteSection(const char* name_utf8) {
    if (!name_utf8) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    for (auto it = g_sections.begin(); it != g_sections.end(); ++it) {
        if ((*it)->name == name_utf8) {
            Log("[api] section deleted: %s (%zu pages)", name_utf8, (*it)->pages.size());
            g_sections.erase(it);
            return true;
        }
    }
    return false;
}

bool RegistryRemovePage(const char* section_utf8, const char* label_utf8) {
    if (!section_utf8 || !label_utf8) return false;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    for (auto& s : g_sections) {
        if (s->name != section_utf8) continue;
        for (auto p = s->pages.begin(); p != s->pages.end(); ++p) {
            if (p->label == label_utf8) {
                Log("[api] page removed: %s / %s", section_utf8, label_utf8);
                s->pages.erase(p);
                return true;
            }
        }
        return false;
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
