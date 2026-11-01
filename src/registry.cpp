#include "registry.h"
#include "common.h"
#include <set>

namespace pbzfm {

namespace {
std::recursive_mutex g_mtx;
std::vector<std::unique_ptr<Section>> g_sections;
thread_local Section* tls_current = nullptr;

std::mutex g_hotkeyMtx;
std::vector<HotkeyReg> g_hotkeys;

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
            tls_current = s.get();
            return s.get();
        }
    }
    auto s = std::make_unique<Section>();
    s->name = name_utf8 ? name_utf8 : "?";
    Section* raw = s.get();
    g_sections.push_back(std::move(s));
    tls_current = raw;
    Log("[api] section registered: %s", raw->name.c_str());
    return raw;
}

Section* RegistryCurrentSection() { return tls_current; }

void RegistryAddPage(Section* section, const Page& page) {
    if (!section) return;
    std::lock_guard<std::recursive_mutex> lock(g_mtx);
    section->pages.push_back(page);
    Log("[api] page registered: %s / %s%s", section->name.c_str(), page.label.c_str(),
        page.schemaCfg ? " (schema)" : " (custom)");
}

void RegistryAddHotkey(const HotkeyReg& reg) {
    std::lock_guard<std::mutex> lock(g_hotkeyMtx);
    g_hotkeys.push_back(reg);
    Log("[api] hotkey registered: vk=0x%02X", reg.vk);
}

bool RegistryDispatchHotkey(unsigned vk, bool up) {
    static std::set<unsigned> down;   // hook thread only
    std::vector<HotkeyReg> fired;
    {
        std::lock_guard<std::mutex> lock(g_hotkeyMtx);
        for (const auto& h : g_hotkeys) {
            if (h.vk == vk) fired.push_back(h);
        }
    }
    if (fired.empty()) return false;
    if (up) {
        down.erase(vk);
        return true;
    }
    if (down.count(vk)) return true;  // held: swallow repeats, fire once per press
    down.insert(vk);
    for (const auto& h : fired) h.fn(h.ud);
    return true;
}

} // namespace pbzfm
