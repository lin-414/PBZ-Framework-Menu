// Implementation of the PBZFM C API (sdk/pbzfm.h). Everything exported here
// resolves through GetProcAddress by client mods; keep names and signatures
// stable - PBZFM_ABI_VERSION gates breaking changes.
#include "pbzfm.h"

#include "common.h"
#include "registry.h"
#include "schema.h"

#include <imgui.h>
#include <algorithm>
#include <cstring>

using namespace pbzfm;

extern "C" {

__declspec(dllexport) int PBZFM_GetAbiVersion(void) {
    return PBZFM_ABI_VERSION;
}

__declspec(dllexport) int PBZFM_IsMenuOpen(void) {
    return g_menuOpen.load(std::memory_order_relaxed) ? 1 : 0;
}

// ---- sections & pages ----

__declspec(dllexport) void PBZFM_SetSection(const char* name_utf8) {
    if (!name_utf8) return;
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    RegistryGetOrCreateSection(name_utf8);
}

__declspec(dllexport) void PBZFM_AddSectionItem(const char* label_utf8, PBZFM_RenderFn fn, void* userdata) {
    if (!label_utf8 || !fn) return;
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    Section* s = RegistryCurrentSection();
    if (!s) s = RegistryGetOrCreateSection("?");
    Page p;
    p.label = label_utf8;
    p.fn = fn;
    p.ud = userdata;
    RegistryAddPage(s, p);
}

// ---- schema ----

__declspec(dllexport) PBZFM_Config PBZFM_ConfigCreate(const char* store_utf8, const char* page_label_utf8) {
    return (PBZFM_Config)SchemaCreateConfig(store_utf8, page_label_utf8);
}

__declspec(dllexport) void PBZFM_ConfigAddGroup(PBZFM_Config cfg, const char* label_utf8) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !label_utf8) return;
    SchemaItem it;
    it.type = SCH_GROUP;
    it.label = label_utf8;
    c->items.push_back(std::move(it));
}

__declspec(dllexport) void PBZFM_ConfigAddBool(PBZFM_Config cfg, const char* key, const char* label_utf8, int def) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key || !label_utf8) return;
    SchemaItem it;
    it.type = SCH_BOOL;
    it.key = key;
    it.label = label_utf8;
    it.ival = def ? 1 : 0;
    c->items.push_back(std::move(it));
}

__declspec(dllexport) void PBZFM_ConfigAddInt(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                              int def, int mn, int mx) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key || !label_utf8 || mx < mn) return;
    SchemaItem it;
    it.type = SCH_INT;
    it.key = key;
    it.label = label_utf8;
    it.ival = std::clamp(def, mn, mx);
    it.mn = (float)mn;
    it.mx = (float)mx;
    it.step = 1.f;
    c->items.push_back(std::move(it));
}

__declspec(dllexport) void PBZFM_ConfigAddFloat(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                                float def, float mn, float mx, float step, const char* fmt_utf8) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key || !label_utf8 || mx < mn) return;
    SchemaItem it;
    it.type = SCH_FLOAT;
    it.key = key;
    it.label = label_utf8;
    it.fval = std::clamp(def, mn, mx);
    it.mn = mn;
    it.mx = mx;
    it.step = step > 0.f ? step : 1.f;
    it.fmt = fmt_utf8 ? fmt_utf8 : "%.3f";
    c->items.push_back(std::move(it));
}

__declspec(dllexport) void PBZFM_ConfigAddEnum(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                               int def, const char* const* options_utf8, int count) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key || !label_utf8 || !options_utf8 || count <= 0) return;
    SchemaItem it;
    it.type = SCH_ENUM;
    it.key = key;
    it.label = label_utf8;
    it.mn = 0.f;
    it.mx = (float)(count - 1);
    for (int i = 0; i < count; ++i) {
        if (options_utf8[i]) it.options.emplace_back(options_utf8[i]);
    }
    if (it.options.empty()) return;
    it.ival = std::clamp((long long)def, 0LL, (long long)it.options.size() - 1);
    c->items.push_back(std::move(it));
}

__declspec(dllexport) void PBZFM_ConfigAddKey(PBZFM_Config cfg, const char* key, const char* label_utf8, unsigned defVk) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key || !label_utf8) return;
    SchemaItem it;
    it.type = SCH_KEY;
    it.key = key;
    it.label = label_utf8;
    it.ival = defVk;
    c->items.push_back(std::move(it));
}

__declspec(dllexport) void PBZFM_ConfigOnChange(PBZFM_Config cfg, PBZFM_ChangeFn fn, void* userdata) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c) return;
    c->changeFn = fn;
    c->changeUd = userdata;
}

__declspec(dllexport) void PBZFM_ConfigSave(PBZFM_Config cfg) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c) return;
    SchemaSaveConfigNow(c);
}

#define PBZFM_GETTER(type, name, field)                                              \
    __declspec(dllexport) type PBZFM_##name(PBZFM_Config cfg, const char* key) {     \
        ConfigDef* c = SchemaValidate(cfg);                                          \
        if (!c) return (type)0;                                                      \
        SchemaItem* it = c->Find(key);                                               \
        return it ? (type)it->field : (type)0;                                       \
    }
PBZFM_GETTER(int, GetBool, ival)
PBZFM_GETTER(float, GetFloat, fval)
PBZFM_GETTER(int, GetInt, ival)
PBZFM_GETTER(int, GetEnum, ival)
PBZFM_GETTER(unsigned, GetKey, ival)
#undef PBZFM_GETTER

#define PBZFM_SETTER(name, type, field, isFloat)                                     \
    __declspec(dllexport) void PBZFM_##name(PBZFM_Config cfg, const char* key, type v) { \
        ConfigDef* c = SchemaValidate(cfg);                                          \
        if (!c || !key) return;                                                      \
        SchemaItem* it = c->Find(key);                                               \
        if (!it) return;                                                             \
        if (isFloat) it->fval = v; else it->ival = (long long)v;                     \
        c->dirty = true;                                                             \
        c->FireChange(key);                                                          \
    }
__declspec(dllexport) void PBZFM_SetBool(PBZFM_Config cfg, const char* key, int v) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key) { Log("[api] SetBool: invalid config"); return; }
    SchemaItem* it = c->Find(key);
    if (!it) { Log("[api] SetBool: key not found: %s", key); return; }
    it->ival = (long long)v;
    c->dirty = true;
    Log("[api] SetBool %s = %d (firing change)", key, v);
    c->FireChange(key);
}
PBZFM_SETTER(SetFloat, float, fval, true)
PBZFM_SETTER(SetInt, int, ival, false)
PBZFM_SETTER(SetEnum, int, ival, false)
PBZFM_SETTER(SetKey, unsigned, ival, false)
#undef PBZFM_SETTER

// ---- hotkeys ----

__declspec(dllexport) void PBZFM_AddHotkey(unsigned vk, PBZFM_HotkeyFn fn, void* userdata) {
    if (!fn) return;
    HotkeyReg reg{ vk, fn, userdata };
    RegistryAddHotkey(reg);
}

// ---- widget API ----

__declspec(dllexport) void PBZFM_Text(const char* utf8) {
    ImGui::TextUnformatted(utf8 ? utf8 : "");
}

__declspec(dllexport) void PBZFM_TextDisabled(const char* utf8) {
    ImGui::TextDisabled("%s", utf8 ? utf8 : "");
}

__declspec(dllexport) void PBZFM_Separator(void) {
    ImGui::Separator();
}

__declspec(dllexport) void PBZFM_SameLine(void) {
    ImGui::SameLine();
}

__declspec(dllexport) int PBZFM_Button(const char* utf8) {
    return ImGui::Button(utf8 ? utf8 : "") ? 1 : 0;
}

__declspec(dllexport) int PBZFM_Checkbox(const char* utf8, int* v) {
    if (!utf8 || !v) return 0;
    bool b = *v != 0;
    bool changed = ImGui::Checkbox(utf8, &b);
    *v = b ? 1 : 0;
    return changed ? 1 : 0;
}

__declspec(dllexport) int PBZFM_SliderFloat(const char* utf8, float* v, float mn, float mx, const char* fmt_utf8) {
    if (!utf8 || !v) return 0;
    return ImGui::SliderFloat(utf8, v, mn, mx, fmt_utf8 ? fmt_utf8 : "%.3f") ? 1 : 0;
}

__declspec(dllexport) int PBZFM_SliderInt(const char* utf8, int* v, int mn, int mx) {
    if (!utf8 || !v) return 0;
    return ImGui::SliderInt(utf8, v, mn, mx) ? 1 : 0;
}

__declspec(dllexport) int PBZFM_Combo(const char* utf8, int* idx, const char* const* items_utf8, int count) {
    if (!utf8 || !idx || !items_utf8 || count <= 0) return 0;
    return ImGui::Combo(utf8, idx, items_utf8, count) ? 1 : 0;
}

__declspec(dllexport) void PBZFM_LastItemRect(float out_xyxy[4]) {
    if (!out_xyxy) return;
    ImVec2 a = ImGui::GetItemRectMin();
    ImVec2 b = ImGui::GetItemRectMax();
    out_xyxy[0] = a.x;
    out_xyxy[1] = a.y;
    out_xyxy[2] = b.x;
    out_xyxy[3] = b.y;
}

__declspec(dllexport) void* PBZFM_GetImGuiContext(void) {
    return ImGui::GetCurrentContext();
}

// ---- automation helpers (menu geometry, overlay-client pixels) ----

__declspec(dllexport) int PBZFM_GetSectionCount(void) {
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    return (int)RegistrySections().size();
}

__declspec(dllexport) int PBZFM_GetSectionNameUtf8(int index, char* buf, int bufLen) {
    if (!buf || bufLen <= 0) return 0;
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    if (index < 0 || index >= (int)RegistrySections().size()) return 0;
    const std::string& n = RegistrySections()[index]->name;
    int len = (int)n.size();
    if (len >= bufLen) len = bufLen - 1;
    memcpy(buf, n.data(), (size_t)len);
    buf[len] = 0;
    return len;
}

__declspec(dllexport) int PBZFM_GetSectionRect(int index, float out_xyxy[4]) {
    if (!out_xyxy) return 0;
    return RegistryGetSectionRect(index, out_xyxy);
}

__declspec(dllexport) int PBZFM_GetLastSchemaItemRect(float out_xyxy[4]) {
    return SchemaGetLastItemRect(out_xyxy);
}

} // extern "C"
