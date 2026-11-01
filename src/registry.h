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

// Lifecycle (v2). Sections are identified by the name passed to SetSection.
// Rename fails on a missing source or an existing target name; deleting a
// section leaves its schema config stores untouched (files stay on disk).
bool RegistryRenameSection(const char* old_utf8, const char* new_utf8);
bool RegistryDeleteSection(const char* name_utf8);
bool RegistryRemovePage(const char* section_utf8, const char* label_utf8);

// Hotkeys fire from the hook thread while the menu is closed, and stay live
// (dispatch-then-swallow) while it is open. mods is a PBZFM_MOD_* bitmask and
// must match exactly. RegistryAddHotkey returns the registration id (>0) used
// by RegistryRemoveHotkey; the id lives in the vector's payload.
int  RegistryAddHotkey(unsigned vk, unsigned mods, PBZFM_HotkeyFn fn, void* ud);
bool RegistryRemoveHotkey(int id);
// Called from the keyboard hook thread for every key down/up. mods is the
// hook's tracked modifier state. Returns true when (vk, mods) belongs to a
// registered hotkey (the caller then swallows it). Fires callbacks on down
// edges only.
bool RegistryDispatchHotkey(unsigned vk, bool up, unsigned mods);

// Menu open/close events (v2), dispatched on the render thread once per
// transition. Ids are the same flavor as hotkey ids (monotonic, >0).
int  RegistryAddMenuEvent(PBZFM_MenuEventFn fn, void* ud);
void RegistryRemoveMenuEvent(int id);
void RegistryDispatchMenuEvent(bool open);

// Sidebar entry rects (overlay-client pixels), recorded by the menu each
// frame and read by automation exports.
void RegistryResetSectionRects();
void RegistryRecordSectionRect(int index, const float xyxy[4]);
int  RegistryGetSectionRect(int index, float out_xyxy[4]);

} // namespace pbzfm
