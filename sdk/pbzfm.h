/*
 * PBZFM SDK - C API for the PBZ Menu Framework (Phantom Blade Zero mod menu)
 * ============================================================================
 * Include this header from your UE4SS C++ DLL mod (or any DLL loaded into the
 * game process) to register settings pages into the in-game menu.
 *
 * Zero build dependencies: everything is resolved through GetProcAddress with
 * the inline PBZFM_LoadApi helper below (or call the PBZFM_* functions by
 * linking against an import lib if you generate one).
 *
 * License: MIT. Client mods may include this header without licensing their
 * own source under the MIT license - see the notice above PBZFM_LoadApi and
 * the repository LICENSE file.
 *
 * Versioning / compatibility policy
 * ---------------------------------
 *   PBZFM_ABI_VERSION  minimum framework API version this header requires.
 *                      Bumped only when a previously-REQUIRED function changes
 *                      incompatibly or is removed.
 *   PBZFM_API_VERSION  API version of the framework build this header ships
 *                      with (what PBZFM_GetAbiVersion() reports at runtime).
 *
 * The required core (everything needed by PBZFM_ABI_VERSION 1) resolves hard:
 * if the running framework lacks any of it, PBZFM_LoadApi fails and the mod
 * runs without the menu. Everything added later resolves OPTIONALLY - the
 * struct field stays NULL when the running framework predates the feature, so
 * a client built against a newer header still runs on an older framework.
 * ALWAYS null-check optional members before calling them:
 *
 *   if (api.RemoveHotkey) api.RemoveHotkey(id);
 *
 * Existing export names and signatures are never changed once shipped.
 *
 * Quick start
 * -----------
 *   PBZFM_Api pbzfm;
 *   if (!PBZFM_LoadApi(&pbzfm)) return;   // framework not loaded (yet); retry
 *   pbzfm.SetSection("My Mod");           // sidebar entry
 *
 *   // schema page: declare items, the framework renders + persists them
 *   PBZFM_Config cfg = pbzfm.ConfigCreate("mymod", "Settings");
 *   pbzfm.ConfigAddBool(cfg, "enable", "Enable effects", 1);
 *   pbzfm.ConfigAddFloat(cfg, "fov", "FOV", 90.f, 15.f, 150.f, 1.f, "%.0f");
 *   pbzfm.ConfigAddKey(cfg, "shot", "Screenshot key", VK_F9);
 *   pbzfm.ConfigOnChange(cfg, OnChange, nullptr);   // applied live in-game
 *
 *   // custom page: draw with the widget API (runs in the framework's frame)
 *   pbzfm.AddSectionItem("About", MyAboutPage, nullptr);
 *
 *   // gameplay hotkey (fires while the menu is closed, key is swallowed)
 *   pbzfm.AddHotkey(VK_F9, OnHotkey, nullptr);
 *   // v2 modifier chord: fires on Ctrl+Alt+P; handle returned for removal
 *   int hk = 0;
 *   if (pbzfm.AddHotkeyEx)
 *       hk = pbzfm.AddHotkeyEx('P', PBZFM_MOD_CTRL | PBZFM_MOD_ALT, OnChord, nullptr);
 *
 * Threading contract
 * ------------------
 *  - Registration: any thread, but NOT inside DllMain (the framework may not
 *    be loaded yet; poll PBZFM_LoadApi() from your own thread).
 *  - Render callbacks: framework render thread, only while the menu is open
 *    and your page is selected. Keep them fast; never block.
 *  - Hotkey callbacks: framework input hook thread; marshal yourself if you
 *    need the game thread.
 *  - Change callbacks: framework render thread, on every committed edit.
 *  - Menu event callbacks (v2): framework render thread, once per menu
 *    open/close transition.
 *
 * Persistence
 * -----------
 * Schema values are owned by the framework and stored at
 *   ue4ss\Mods\PBZFM\data\<store>.json
 * Edits made in the menu update the store and fire your change callback so
 * settings apply live. The SetXxx() functions write programmatically from
 * anywhere (same persistence + callback semantics).
 */
#ifndef PBZFM_SDK_H
#define PBZFM_SDK_H

#include <windows.h>

#define PBZFM_ABI_VERSION 1   /* minimum API version this header requires  */
#define PBZFM_API_VERSION 3   /* API version of the framework build        */

/* Modifier bits for AddHotkeyEx / ConfigAddKeyCombo (v2). A chord fires only
 * when the held modifiers match exactly - holding an extra Shift does not
 * trigger a Ctrl+Alt+P binding (same strictness as the framework's own
 * screenshot passthrough chords). */
#define PBZFM_MOD_CTRL  0x1u
#define PBZFM_MOD_SHIFT 0x2u
#define PBZFM_MOD_ALT   0x4u
#define PBZFM_MOD_WIN   0x8u

/* When building the framework itself the declarations export; client mods
 * include this header with PBZFM_EXPORTS undefined and get plain
 * declarations (they call through the PBZFM_Api pointers or GetProcAddress). */
#if defined(PBZFM_EXPORTS)
#define PBZFM_API __declspec(dllexport)
#else
#define PBZFM_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PBZFM_RenderFn)(void* userdata);
typedef void (*PBZFM_ChangeFn)(const char* key, void* userdata);
typedef void (*PBZFM_HotkeyFn)(void* userdata);
/* open: 1 when the menu just opened, 0 when it just closed (render thread). */
typedef void (*PBZFM_MenuEventFn)(int open, void* userdata);

typedef void* PBZFM_Config; /* opaque handle from PBZFM_ConfigCreate */

/* ---- framework discovery ------------------------------------------------ */

PBZFM_API int  PBZFM_GetAbiVersion(void);  /* running framework's API version */
PBZFM_API int  PBZFM_IsMenuOpen(void);     /* 1 while the overlay menu is visible */

/* ---- sections & pages --------------------------------------------------- */

PBZFM_API void PBZFM_SetSection(const char* name_utf8);
PBZFM_API void PBZFM_AddSectionItem(const char* label_utf8, PBZFM_RenderFn fn, void* userdata);

/* v2 lifecycle: targets are PATHS (see SetSectionPath); a single segment
 * resolves to a top-level section, which is exactly the v1 "by name" call.
 * Renaming renames the node's last segment (a single-segment name, no '/'),
 * fails on sibling collisions; deleting removes the node's whole subtree but
 * leaves its schema config stores on disk (values reload if the mod
 * re-registers the store). After deleting or renaming the node that is your
 * calling thread's current section, call SetSection/SetSectionPath again
 * before ConfigCreate/AddSectionItem. Returns 1 on success. */
PBZFM_API int  PBZFM_RenameSection(const char* target_path, const char* new_utf8);
PBZFM_API int  PBZFM_DeleteSection(const char* target_path);
PBZFM_API int  PBZFM_RemoveSectionItem(const char* target_path, const char* label_utf8);

/* v3 nested paths: "Mod/Category/Sub" - '/' separates segments, a backslash
 * escapes the next byte (a literal '/' in a segment is "\/"). A single-segment
 * path is a top-level section, i.e. SetSectionPath("A") == SetSection("A").
 * The node at the path carries the page; missing intermediates are created
 * empty, so the sidebar grows a tree. The in-game sidebar renders one flat
 * row for a node that has a single page and no children - only real trees
 * grow carets. */
PBZFM_API void PBZFM_SetSectionPath(const char* path_utf8);
PBZFM_API void PBZFM_AddSectionItemPath(const char* path_utf8, PBZFM_RenderFn fn, void* userdata);
PBZFM_API PBZFM_Config PBZFM_ConfigCreatePath(const char* store_utf8, const char* path_utf8);

/* ---- schema-backed settings --------------------------------------------- */

PBZFM_API PBZFM_Config PBZFM_ConfigCreate(const char* store_utf8, const char* page_label_utf8);

PBZFM_API void PBZFM_ConfigAddGroup(PBZFM_Config cfg, const char* label_utf8);
PBZFM_API void PBZFM_ConfigAddBool(PBZFM_Config cfg, const char* key, const char* label_utf8, int def);
PBZFM_API void PBZFM_ConfigAddInt(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                  int def, int mn, int mx);
PBZFM_API void PBZFM_ConfigAddFloat(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                    float def, float mn, float mx, float step, const char* fmt_utf8);
/* options_utf8: array of count utf8 strings; the stored value is the option index */
PBZFM_API void PBZFM_ConfigAddEnum(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                   int def, const char* const* options_utf8, int count);
/* keybind: value is a Win32 virtual-key code; edited via in-menu key capture */
PBZFM_API void PBZFM_ConfigAddKey(PBZFM_Config cfg, const char* key, const char* label_utf8, unsigned defVk);
/* v2 modifier chord keybind: stored value is vk | (mods << 8); captured in the
 * menu by holding the modifiers, then pressing the final key. */
PBZFM_API void PBZFM_ConfigAddKeyCombo(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                       unsigned defVk, unsigned defMods);

PBZFM_API void PBZFM_ConfigOnChange(PBZFM_Config cfg, PBZFM_ChangeFn fn, void* userdata);
PBZFM_API void PBZFM_ConfigSave(PBZFM_Config cfg);

PBZFM_API int      PBZFM_GetBool(PBZFM_Config cfg, const char* key);
PBZFM_API float    PBZFM_GetFloat(PBZFM_Config cfg, const char* key);
PBZFM_API int      PBZFM_GetInt(PBZFM_Config cfg, const char* key);
PBZFM_API int      PBZFM_GetEnum(PBZFM_Config cfg, const char* key);
PBZFM_API unsigned PBZFM_GetKey(PBZFM_Config cfg, const char* key);      /* vk (combo: vk only) */
PBZFM_API unsigned PBZFM_GetKeyMods(PBZFM_Config cfg, const char* key);  /* v2: PBZFM_MOD_* bits */
PBZFM_API void     PBZFM_SetBool(PBZFM_Config cfg, const char* key, int v);
PBZFM_API void     PBZFM_SetFloat(PBZFM_Config cfg, const char* key, float v);
PBZFM_API void     PBZFM_SetInt(PBZFM_Config cfg, const char* key, int v);
PBZFM_API void     PBZFM_SetEnum(PBZFM_Config cfg, const char* key, int idx);
PBZFM_API void     PBZFM_SetKey(PBZFM_Config cfg, const char* key, unsigned vk);
PBZFM_API void     PBZFM_SetKeyCombo(PBZFM_Config cfg, const char* key, unsigned vk, unsigned mods);
/* v2 human-readable name: "F9", "Ctrl+Alt+P"; writes into buf, returns length. */
PBZFM_API int      PBZFM_KeyName(unsigned vk, unsigned mods, char* buf, int bufLen);

/* ---- gameplay hotkeys ---------------------------------------------------- */

PBZFM_API void PBZFM_AddHotkey(unsigned vk, PBZFM_HotkeyFn fn, void* userdata);
/* v2: chord hotkey with exact modifier match; returns a handle for
 * PBZFM_RemoveHotkey (0 on failure). Callbacks run on the input hook thread. */
PBZFM_API int  PBZFM_AddHotkeyEx(unsigned vk, unsigned mods, PBZFM_HotkeyFn fn, void* userdata);
PBZFM_API void PBZFM_RemoveHotkey(int id);

/* ---- menu events (v2) ---------------------------------------------------- */
/* Fired on the render thread once per transition, after the overlay became
 * (in)visible. Returns an event id for PBZFM_RemoveMenuEvent (0 on failure). */
PBZFM_API int  PBZFM_OnMenuEvent(PBZFM_MenuEventFn fn, void* userdata);
PBZFM_API void PBZFM_RemoveMenuEvent(int id);

/* ---- widget API for custom pages ---------------------------------------- */
/* Only valid inside a render callback; draws into the framework's frame. */

PBZFM_API void PBZFM_Text(const char* utf8);
PBZFM_API void PBZFM_TextDisabled(const char* utf8);
PBZFM_API void PBZFM_Separator(void);
PBZFM_API void PBZFM_SameLine(void);
PBZFM_API int  PBZFM_Button(const char* utf8);
PBZFM_API int  PBZFM_Checkbox(const char* utf8, int* v);
PBZFM_API int  PBZFM_SliderFloat(const char* utf8, float* v, float mn, float mx, const char* fmt_utf8);
PBZFM_API int  PBZFM_SliderInt(const char* utf8, int* v, int mn, int mx);
PBZFM_API int  PBZFM_Combo(const char* utf8, int* idx, const char* const* items_utf8, int count);
/* rect of the last item in overlay-client pixels: x0, y0, x1, y1 */
PBZFM_API void PBZFM_LastItemRect(float out_xyxy[4]);

/* v2 widgets: InputText edits buf in place (returns 1 when the value changed;
 * buf is always null-terminated). Selectable returns 1 when clicked. */
PBZFM_API int  PBZFM_InputText(const char* label_utf8, char* buf, unsigned bufSize);
PBZFM_API int  PBZFM_CollapsingHeader(const char* label_utf8);
PBZFM_API int  PBZFM_Selectable(const char* label_utf8, int selected);
PBZFM_API void PBZFM_ProgressBar(float fraction, float width, const char* overlay_utf8);
PBZFM_API int  PBZFM_ColorEdit3(const char* label_utf8, float rgb[3]);

/* v2 textures: LoadTexture decodes PNG/JPG/BMP/TGA (stb_image) and uploads on
 * the render thread; the handle is valid immediately but renders empty until
 * the load completes. Relative paths resolve against the framework mod root
 * (ue4ss\Mods\PBZFM); absolute paths are used as-is. DisposeTexture frees
 * the GPU memory when you are done with it. */
PBZFM_API void* PBZFM_LoadTexture(const char* path_utf8);
PBZFM_API void  PBZFM_DisposeTexture(void* texture);
PBZFM_API void  PBZFM_Image(void* texture, float w, float h);

/* ---- advanced: raw ImGui escape hatch ----------------------------------- */
/* Returns the framework's ImGuiContext*. If your mod statically links the
 * SAME ImGui version the framework was built with, you may call
 * ImGui::SetCurrentContext(PBZFM_GetImGuiContext()) inside your render
 * callback and use ImGui directly. Version mismatches corrupt memory. */
PBZFM_API void* PBZFM_GetImGuiContext(void);

/* ---- automation helpers (test tooling; overlay-client pixels) ------------ */
/* For test tooling and overlay trackers; regular mods do not need them.
 * Rects are x0,y0,x1,y1 in overlay-client pixels, valid while the menu is
 * open (the menu re-records them every frame). */

PBZFM_API int  PBZFM_GetSectionCount(void);
/* name utf8, truncated to bufLen-1; returns the name length */
PBZFM_API int  PBZFM_GetSectionNameUtf8(int index, char* buf, int bufLen);
/* sidebar entry rect; returns 0 when index is out of range */
PBZFM_API int  PBZFM_GetSectionRect(int index, float out_xyxy[4]);
/* rect of the most recently rendered schema item */
PBZFM_API int  PBZFM_GetLastSchemaItemRect(float out_xyxy[4]);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ==========================================================================
 * Inline loader: fills a PBZFM_Api struct with resolved function pointers.
 *
 * MIT License - Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this header to deal in it without restriction,
 * including the rights to use, copy, modify, merge, publish and distribute
 * it in their own mods, subject to keeping this notice. (Full text in the
 * repository LICENSE; this header intentionally imposes no obligations on
 * the mods that include it.)
 * ========================================================================== */

struct PBZFM_Api {
    /* ---- required core (ABI version 1) ---- */
    int  (*GetAbiVersion)(void);
    int  (*IsMenuOpen)(void);
    void (*SetSection)(const char* name_utf8);
    void (*AddSectionItem)(const char* label_utf8, PBZFM_RenderFn fn, void* userdata);
    PBZFM_Config (*ConfigCreate)(const char* store_utf8, const char* page_label_utf8);
    void (*ConfigAddGroup)(PBZFM_Config, const char*);
    void (*ConfigAddBool)(PBZFM_Config, const char*, const char*, int);
    void (*ConfigAddInt)(PBZFM_Config, const char*, const char*, int, int, int);
    void (*ConfigAddFloat)(PBZFM_Config, const char*, const char*, float, float, float, float, const char*);
    void (*ConfigAddEnum)(PBZFM_Config, const char*, const char*, int, const char* const*, int);
    void (*ConfigAddKey)(PBZFM_Config, const char*, const char*, unsigned);
    void (*ConfigOnChange)(PBZFM_Config, PBZFM_ChangeFn, void*);
    void (*ConfigSave)(PBZFM_Config);
    int      (*GetBool)(PBZFM_Config, const char*);
    float    (*GetFloat)(PBZFM_Config, const char*);
    int      (*GetInt)(PBZFM_Config, const char*);
    int      (*GetEnum)(PBZFM_Config, const char*);
    unsigned (*GetKey)(PBZFM_Config, const char*);
    void (*SetBool)(PBZFM_Config, const char*, int);
    void (*SetFloat)(PBZFM_Config, const char*, float);
    void (*SetInt)(PBZFM_Config, const char*, int);
    void (*SetEnum)(PBZFM_Config, const char*, int);
    void (*SetKey)(PBZFM_Config, const char*, unsigned);
    void (*AddHotkey)(unsigned, PBZFM_HotkeyFn, void*);
    void (*Text)(const char*);
    void (*TextDisabled)(const char*);
    void (*Separator)(void);
    void (*SameLine)(void);
    int  (*Button)(const char*);
    int  (*Checkbox)(const char*, int*);
    int  (*SliderFloat)(const char*, float*, float, float, const char*);
    int  (*SliderInt)(const char*, int*, int, int);
    int  (*Combo)(const char*, int*, const char* const*, int);
    void (*LastItemRect)(float out_xyxy[4]);
    void* (*GetImGuiContext)(void);
    /* ---- v2 additions (optional; NULL when the framework predates them) ---- */
    int      (*RenameSection)(const char*, const char*);
    int      (*DeleteSection)(const char*);
    int      (*RemoveSectionItem)(const char*, const char*);
    int      (*AddHotkeyEx)(unsigned vk, unsigned mods, PBZFM_HotkeyFn, void*);
    void     (*RemoveHotkey)(int id);
    int      (*OnMenuEvent)(PBZFM_MenuEventFn, void*);
    void     (*RemoveMenuEvent)(int id);
    void     (*ConfigAddKeyCombo)(PBZFM_Config, const char*, const char*, unsigned, unsigned);
    unsigned (*GetKeyMods)(PBZFM_Config, const char*);
    void     (*SetKeyCombo)(PBZFM_Config, const char*, unsigned, unsigned);
    int      (*KeyName)(unsigned vk, unsigned mods, char* buf, int bufLen);
    void*    (*LoadTexture)(const char* path_utf8);
    void     (*DisposeTexture)(void* texture);
    void     (*Image)(void* texture, float w, float h);
    int      (*InputText)(const char*, char*, unsigned);
    int      (*CollapsingHeader)(const char*);
    int      (*Selectable)(const char*, int);
    void     (*ProgressBar)(float fraction, float width, const char* overlay_utf8);
    int      (*ColorEdit3)(const char*, float rgb[3]);
    const char* (*VersionString)(void);
    /* ---- v3 additions (optional; NULL when the framework predates them) ---- */
    void     (*SetSectionPath)(const char* path_utf8);
    void     (*AddSectionItemPath)(const char* path_utf8, PBZFM_RenderFn, void*);
    PBZFM_Config (*ConfigCreatePath)(const char* store_utf8, const char* path_utf8);
};

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves all PBZFM functions. Returns 1 on success; out is zeroed.
 * Required core (v1) must resolve completely and the framework API version
 * must be >= PBZFM_ABI_VERSION; v2+ functions resolve optionally and may be
 * NULL (null-check before use). */
static int PBZFM_LoadApi(struct PBZFM_Api* out) {
    HMODULE m = GetModuleHandleW(L"PBZFM.dll");
    if (!m || !out) return 0;
    memset(out, 0, sizeof(*out));
#define PBZFM_GET(field, name) do { *(FARPROC*)&out->field = GetProcAddress(m, name); if (!out->field) return 0; } while (0)
#define PBZFM_GET_OPT(field, name) do { *(FARPROC*)&out->field = GetProcAddress(m, name); } while (0)
    PBZFM_GET(GetAbiVersion, "PBZFM_GetAbiVersion");
    PBZFM_GET(IsMenuOpen, "PBZFM_IsMenuOpen");
    PBZFM_GET(SetSection, "PBZFM_SetSection");
    PBZFM_GET(AddSectionItem, "PBZFM_AddSectionItem");
    PBZFM_GET(ConfigCreate, "PBZFM_ConfigCreate");
    PBZFM_GET(ConfigAddGroup, "PBZFM_ConfigAddGroup");
    PBZFM_GET(ConfigAddBool, "PBZFM_ConfigAddBool");
    PBZFM_GET(ConfigAddInt, "PBZFM_ConfigAddInt");
    PBZFM_GET(ConfigAddFloat, "PBZFM_ConfigAddFloat");
    PBZFM_GET(ConfigAddEnum, "PBZFM_ConfigAddEnum");
    PBZFM_GET(ConfigAddKey, "PBZFM_ConfigAddKey");
    PBZFM_GET(ConfigOnChange, "PBZFM_ConfigOnChange");
    PBZFM_GET(ConfigSave, "PBZFM_ConfigSave");
    PBZFM_GET(GetBool, "PBZFM_GetBool");
    PBZFM_GET(GetFloat, "PBZFM_GetFloat");
    PBZFM_GET(GetInt, "PBZFM_GetInt");
    PBZFM_GET(GetEnum, "PBZFM_GetEnum");
    PBZFM_GET(GetKey, "PBZFM_GetKey");
    PBZFM_GET(SetBool, "PBZFM_SetBool");
    PBZFM_GET(SetFloat, "PBZFM_SetFloat");
    PBZFM_GET(SetInt, "PBZFM_SetInt");
    PBZFM_GET(SetEnum, "PBZFM_SetEnum");
    PBZFM_GET(SetKey, "PBZFM_SetKey");
    PBZFM_GET(AddHotkey, "PBZFM_AddHotkey");
    PBZFM_GET(Text, "PBZFM_Text");
    PBZFM_GET(TextDisabled, "PBZFM_TextDisabled");
    PBZFM_GET(Separator, "PBZFM_Separator");
    PBZFM_GET(SameLine, "PBZFM_SameLine");
    PBZFM_GET(Button, "PBZFM_Button");
    PBZFM_GET(Checkbox, "PBZFM_Checkbox");
    PBZFM_GET(SliderFloat, "PBZFM_SliderFloat");
    PBZFM_GET(SliderInt, "PBZFM_SliderInt");
    PBZFM_GET(Combo, "PBZFM_Combo");
    PBZFM_GET(LastItemRect, "PBZFM_LastItemRect");
    PBZFM_GET(GetImGuiContext, "PBZFM_GetImGuiContext");
    /* v2: optional - resolve, but do not fail when missing. */
    PBZFM_GET_OPT(RenameSection, "PBZFM_RenameSection");
    PBZFM_GET_OPT(DeleteSection, "PBZFM_DeleteSection");
    PBZFM_GET_OPT(RemoveSectionItem, "PBZFM_RemoveSectionItem");
    PBZFM_GET_OPT(AddHotkeyEx, "PBZFM_AddHotkeyEx");
    PBZFM_GET_OPT(RemoveHotkey, "PBZFM_RemoveHotkey");
    PBZFM_GET_OPT(OnMenuEvent, "PBZFM_OnMenuEvent");
    PBZFM_GET_OPT(RemoveMenuEvent, "PBZFM_RemoveMenuEvent");
    PBZFM_GET_OPT(ConfigAddKeyCombo, "PBZFM_ConfigAddKeyCombo");
    PBZFM_GET_OPT(GetKeyMods, "PBZFM_GetKeyMods");
    PBZFM_GET_OPT(SetKeyCombo, "PBZFM_SetKeyCombo");
    PBZFM_GET_OPT(KeyName, "PBZFM_KeyName");
    PBZFM_GET_OPT(LoadTexture, "PBZFM_LoadTexture");
    PBZFM_GET_OPT(DisposeTexture, "PBZFM_DisposeTexture");
    PBZFM_GET_OPT(Image, "PBZFM_Image");
    PBZFM_GET_OPT(InputText, "PBZFM_InputText");
    PBZFM_GET_OPT(CollapsingHeader, "PBZFM_CollapsingHeader");
    PBZFM_GET_OPT(Selectable, "PBZFM_Selectable");
    PBZFM_GET_OPT(ProgressBar, "PBZFM_ProgressBar");
    PBZFM_GET_OPT(ColorEdit3, "PBZFM_ColorEdit3");
    PBZFM_GET_OPT(VersionString, "PBZFM_Version");
    /* v3: optional - nested-path registration. */
    PBZFM_GET_OPT(SetSectionPath, "PBZFM_SetSectionPath");
    PBZFM_GET_OPT(AddSectionItemPath, "PBZFM_AddSectionItemPath");
    PBZFM_GET_OPT(ConfigCreatePath, "PBZFM_ConfigCreatePath");
#undef PBZFM_GET_OPT
#undef PBZFM_GET
    if (out->GetAbiVersion() < PBZFM_ABI_VERSION) return 0;
    return 1;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PBZFM_SDK_H */
