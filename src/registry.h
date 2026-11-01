#pragma once
// Section/page registry behind the PBZFM C API. Client mods register from
// their own threads; the render thread iterates under the same recursive
// mutex (registration blocks for at most one frame).
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "pbzfm.h"

namespace pbzfm {

struct Page {
    std::string label;
    PBZFM_RenderFn fn = nullptr;   // custom render callback (widget API / raw)
    void* ud = nullptr;
    void* schemaCfg = nullptr;     // ConfigDef* when the page is schema-backed
};

struct Section {
    std::string name;
    std::vector<Page> pages;
};

std::recursive_mutex& RegistryMutex();
std::vector<std::unique_ptr<Section>>& RegistrySections();
Section* RegistryGetOrCreateSection(const char* name_utf8);   // also makes it the calling thread's current
Section* RegistryCurrentSection();                            // calling thread's current (may be null)
void RegistryAddPage(Section* section, const Page& page);

struct HotkeyReg {
    unsigned vk;
    PBZFM_HotkeyFn fn;
    void* ud;
};
void RegistryAddHotkey(const HotkeyReg& reg);
// Called from the keyboard hook thread for every key down/up while the menu
// is closed. Returns true when vk belongs to a registered hotkey (the caller
// then swallows it). Fires callbacks on down edges only.
bool RegistryDispatchHotkey(unsigned vk, bool up);

// Sidebar entry rects (overlay-client pixels), recorded by the menu each
// frame and read by automation exports.
void RegistryResetSectionRects();
void RegistryRecordSectionRect(int index, const float xyxy[4]);
int  RegistryGetSectionRect(int index, float out_xyxy[4]);

} // namespace pbzfm
