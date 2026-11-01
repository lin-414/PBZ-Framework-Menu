#pragma once
// Section tree behind the PBZFM C API. A "section" is a sidebar tree node:
// it carries pages (schema-backed or custom render callbacks) and can have
// child sections (the 0.5.0 nested-path API). Client mods register from their
// own threads; the render thread iterates under the same recursive mutex
// (registration blocks for at most one frame).
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

struct Section {                   // one sidebar tree node
    std::string name;              // single path segment (no '/')
    Section* parent = nullptr;     // null for roots
    std::vector<Page> pages;       // content ON this node (usually 0 or 1)
    std::vector<std::unique_ptr<Section>> children;
};

std::recursive_mutex& RegistryMutex();
std::vector<std::unique_ptr<Section>>& RegistrySections();   // roots
Section* RegistryGetOrCreateSection(const char* name_utf8);  // v1: root by name; also sets it current
Section* RegistryCurrentSection();                           // calling thread's current (may be null)
void RegistryAddPage(Section* section, const Page& page);

// Nested paths (v2/v3): "Mod/Category/Sub" - '/' separates segments, a
// backslash escapes the next byte (a literal '/' in a segment is "\/").
// Missing intermediates are created empty; the node at the path becomes the
// calling thread's current section. RegistryFindPath only looks (used by the
// renderer and by programmatic writes), it never creates.
Section* RegistryGetOrCreatePath(const char* path_utf8);
Section* RegistryFindPath(const char* path_utf8);
std::string RegistryPathOf(const Section* node);   // escaped, '/'-joined

// Lifecycle. Targets are PATHS (a single segment resolves to a root, which is
// exactly the v1 name behavior). Rename renames the node's last segment and
// fails on sibling collisions; deleting a node removes its whole subtree and
// leaves schema config stores untouched (files stay on disk).
bool RegistryRenameSection(const char* target_path, const char* new_name);
bool RegistryDeleteSection(const char* target_path);
bool RegistryRemovePage(const char* target_path, const char* label_utf8);

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

// Sidebar entry rects (overlay-client pixels) for the ROOT rows, recorded by
// the menu each frame and read by automation exports.
void RegistryResetSectionRects();
void RegistryRecordSectionRect(int index, const float xyxy[4]);
int  RegistryGetSectionRect(int index, float out_xyxy[4]);

} // namespace pbzfm
