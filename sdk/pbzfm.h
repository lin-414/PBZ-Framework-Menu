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
 * Quick start
 * -----------
 *   PBZFM_Api pbzfm;
 *   if (!PBZFM_LoadApi(&pbzfm)) return;   // framework not loaded (yet); retry
 *   if (pbzfm.GetAbiVersion() != PBZFM_ABI_VERSION) return;
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
 *
 * Threading contract
 * ------------------
 *  - Registration: any thread, but NOT inside DllMain (the framework may not
 *    be loaded yet; poll PBZFM_IsInstalled() from your own thread).
 *  - Render callbacks: framework render thread, only while the menu is open
 *    and your page is selected. Keep them fast; never block.
 *  - Hotkey callbacks: framework input hook thread; marshal yourself if you
 *    need the game thread.
 *  - Change callbacks: framework render thread, on every committed edit.
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

#define PBZFM_ABI_VERSION 1

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

typedef void* PBZFM_Config; /* opaque handle from PBZFM_ConfigCreate */

/* ---- framework discovery ------------------------------------------------ */

PBZFM_API int  PBZFM_GetAbiVersion(void);
PBZFM_API int  PBZFM_IsMenuOpen(void);  /* 1 while the overlay menu is visible */

/* ---- sections & pages --------------------------------------------------- */

PBZFM_API void PBZFM_SetSection(const char* name_utf8);
PBZFM_API void PBZFM_AddSectionItem(const char* label_utf8, PBZFM_RenderFn fn, void* userdata);

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

PBZFM_API void PBZFM_ConfigOnChange(PBZFM_Config cfg, PBZFM_ChangeFn fn, void* userdata);
PBZFM_API void PBZFM_ConfigSave(PBZFM_Config cfg);

PBZFM_API int      PBZFM_GetBool(PBZFM_Config cfg, const char* key);
PBZFM_API float    PBZFM_GetFloat(PBZFM_Config cfg, const char* key);
PBZFM_API int      PBZFM_GetInt(PBZFM_Config cfg, const char* key);
PBZFM_API int      PBZFM_GetEnum(PBZFM_Config cfg, const char* key);
PBZFM_API unsigned PBZFM_GetKey(PBZFM_Config cfg, const char* key);
PBZFM_API void     PBZFM_SetBool(PBZFM_Config cfg, const char* key, int v);
PBZFM_API void     PBZFM_SetFloat(PBZFM_Config cfg, const char* key, float v);
PBZFM_API void     PBZFM_SetInt(PBZFM_Config cfg, const char* key, int v);
PBZFM_API void     PBZFM_SetEnum(PBZFM_Config cfg, const char* key, int idx);
PBZFM_API void     PBZFM_SetKey(PBZFM_Config cfg, const char* key, unsigned vk);

/* ---- gameplay hotkeys ---------------------------------------------------- */

PBZFM_API void PBZFM_AddHotkey(unsigned vk, PBZFM_HotkeyFn fn, void* userdata);

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
 * ========================================================================== */

struct PBZFM_Api {
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
};

#ifdef __cplusplus
extern "C" {
#endif

/* Resolves all PBZFM functions. Returns 0 on success; out is zeroed and
 * PBZFM_GetAbiVersion is also checked against PBZFM_ABI_VERSION. */
static int PBZFM_LoadApi(struct PBZFM_Api* out) {
    HMODULE m = GetModuleHandleW(L"PBZFM.dll");
    if (!m || !out) return 0;
    memset(out, 0, sizeof(*out));
#define PBZFM_GET(field, name) \
    do { *(FARPROC*)&out->field = GetProcAddress(m, name); if (!out->field) return 0; } while (0)
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
#undef PBZFM_GET
    if (out->GetAbiVersion() != PBZFM_ABI_VERSION) return 0;
    return 1;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PBZFM_SDK_H */
