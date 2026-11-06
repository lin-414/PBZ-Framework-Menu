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

// Review H8: run "resolve/create the node + use it" inside ONE registry critical
// section. `work` receives a Section* that is only valid for the duration of
// the call, which is exactly what keeps a concurrent DeleteSection from freeing
// it between an api-level lookup and the follow-up page/config attach. `work`
// runs under the lock: keep it quick and never wait on another thread that
// calls back into the registry.
template <typename Fn>
bool RegistryWithPath(const char* path_utf8, Fn&& work, std::string* resolvedPath = nullptr) {
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    Section* node = RegistryGetOrCreatePath(path_utf8);
    if (!node) return false;
    work(node);
    if (resolvedPath) *resolvedPath = RegistryPathOf(node);
    return true;
}

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
// Review H6: the LL keyboard hook must never run mod code - one slow callback
// stalls the whole game's input chain. RegistryQueueHotkey takes the same
// match/swallow decision as the old synchronous dispatch but only appends the
// callback to a bounded queue; RegistryFlushQueuedHotkeys() runs those callbacks
// on the RENDER thread (called every loop iteration by overlay.cpp, so a hotkey
// works with the menu open or closed). Queue cap: 32 presses; beyond it new
// presses are dropped with a single logged warning. Flush runs outside the ImGui
// frame, so callbacks must not call ImGui widgets and must not block.
bool RegistryQueueHotkey(unsigned vk, bool up, unsigned mods);
void RegistryFlushQueuedHotkeys();
// Synchronous variant: fires the callback on the CALLING thread. Automation
// tests use it; the hook thread does not (see RegistryQueueHotkey above).
// Returns true when (vk, mods) belongs to a registered hotkey (the caller then
// swallows it). Fires callbacks on down edges only.
bool RegistryDispatchHotkey(unsigned vk, bool up, unsigned mods);

// Menu open/close events (v2), dispatched on the render thread once per
// transition. Ids are the same flavor as hotkey ids (monotonic, >0).
int  RegistryAddMenuEvent(PBZFM_MenuEventFn fn, void* ud);
void RegistryRemoveMenuEvent(int id);
void RegistryDispatchMenuEvent(bool open);

// Review H5 (hot-reload safety): every page / hotkey / menu-event callback is
// checked against the module that owns its entry point. Callbacks whose module
// hot-unloaded are dropped (pages keep their row but lose the fn, hotkeys and
// menu events are erased, schema change callbacks are cleared) so the framework
// can never call into unmapped code. Cheap: run it from the render thread
// before drawing, and from the menu-event dispatch; hotkey dispatch filters the
// matched entries directly so the hook thread never takes the registry lock.
void RegistryPurgeOrphanedCallbacks();

// Bulk entry for a mod manager that unloads everything at once: drops every
// callback not owned by the framework DLL itself. (A hot-unload without this
// call is handled by RegistryPurgeOrphanedCallbacks.)
void RegistryClearModCallbacks();

// api.cpp records each ConfigDef whose change callback it stored, so the sweep
// can clear changeFn of a config owned by a module that unloaded. schema.cpp
// keeps the config lifetime table; the ConfigDef pointer itself stays valid.
void RegistryTrackConfigCallback(void* config);

// Sidebar entry rects (overlay-client pixels) for the ROOT rows, recorded by
// the menu each frame and read by automation exports.
void RegistryResetSectionRects();
void RegistryRecordSectionRect(int index, const float xyxy[4]);
int  RegistryGetSectionRect(int index, float out_xyxy[4]);

} // namespace pbzfm
