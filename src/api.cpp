// Implementation of the PBZFM C API (sdk/pbzfm.h). Everything exported here
// resolves through GetProcAddress by client mods; keep names and signatures
// stable - the required v1 core never changes, new functions are additive
// (versioning policy documented in sdk/pbzfm.h).
#include "pbzfm.h"

#include "common.h"
#include "config.h"
#include "registry.h"
#include "schema.h"
#include "textures.h"

#include <imgui.h>
#include <algorithm>
#include <cstring>

using namespace pbzfm;

extern "C" {

__declspec(dllexport) int PBZFM_GetAbiVersion(void) {
    return PBZFM_API_VERSION;
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

// ---- section lifecycle (v2) ----

__declspec(dllexport) int PBZFM_RenameSection(const char* old_utf8, const char* new_utf8) {
    return RegistryRenameSection(old_utf8, new_utf8) ? 1 : 0;
}

__declspec(dllexport) int PBZFM_DeleteSection(const char* name_utf8) {
    return RegistryDeleteSection(name_utf8) ? 1 : 0;
}

__declspec(dllexport) int PBZFM_RemoveSectionItem(const char* section_utf8, const char* label_utf8) {
    return RegistryRemovePage(section_utf8, label_utf8) ? 1 : 0;
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
    it.ival = defVk & 0xFFu;
    c->items.push_back(std::move(it));
}

__declspec(dllexport) void PBZFM_ConfigAddKeyCombo(PBZFM_Config cfg, const char* key, const char* label_utf8,
                                                   unsigned defVk, unsigned defMods) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key || !label_utf8) return;
    SchemaItem it;
    it.type = SCH_KEYCOMBO;
    it.key = key;
    it.label = label_utf8;
    it.ival = (long long)((defVk & 0xFFu) | ((defMods & 0xFu) << 8));
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
#undef PBZFM_GETTER

__declspec(dllexport) unsigned PBZFM_GetKey(PBZFM_Config cfg, const char* key) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key) return 0;
    SchemaItem* it = c->Find(key);
    return it ? (unsigned)it->ival & 0xFFu : 0u;   // combos store mods in the high bits
}

__declspec(dllexport) unsigned PBZFM_GetKeyMods(PBZFM_Config cfg, const char* key) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key) return 0;
    SchemaItem* it = c->Find(key);
    return it ? ((unsigned)it->ival >> 8) & 0xFu : 0u;
}

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

__declspec(dllexport) void PBZFM_SetKeyCombo(PBZFM_Config cfg, const char* key, unsigned vk, unsigned mods) {
    ConfigDef* c = SchemaValidate(cfg);
    if (!c || !key) return;
    SchemaItem* it = c->Find(key);
    if (!it || it->type != SCH_KEYCOMBO) return;
    it->ival = (long long)((vk & 0xFFu) | ((mods & 0xFu) << 8));
    c->dirty = true;
    c->FireChange(key);
}

__declspec(dllexport) int PBZFM_KeyName(unsigned vk, unsigned mods, char* buf, int bufLen) {
    if (!buf || bufLen <= 0) return 0;
    std::string name = Config::KeyModsToString(vk, mods);
    int len = (int)name.size();
    if (len >= bufLen) len = bufLen - 1;
    memcpy(buf, name.data(), (size_t)len);
    buf[len] = 0;
    return len;
}

// ---- hotkeys ----

__declspec(dllexport) void PBZFM_AddHotkey(unsigned vk, PBZFM_HotkeyFn fn, void* userdata) {
    RegistryAddHotkey(vk, 0, fn, userdata);   // id unused by the v1 entry point
}

__declspec(dllexport) int PBZFM_AddHotkeyEx(unsigned vk, unsigned mods, PBZFM_HotkeyFn fn, void* userdata) {
    return RegistryAddHotkey(vk, mods, fn, userdata);
}

__declspec(dllexport) void PBZFM_RemoveHotkey(int id) {
    RegistryRemoveHotkey(id);
}

// ---- menu events (v2) ----

__declspec(dllexport) int PBZFM_OnMenuEvent(PBZFM_MenuEventFn fn, void* userdata) {
    return RegistryAddMenuEvent(fn, userdata);
}

__declspec(dllexport) void PBZFM_RemoveMenuEvent(int id) {
    RegistryRemoveMenuEvent(id);
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

__declspec(dllexport) int PBZFM_InputText(const char* utf8, char* buf, unsigned bufSize) {
    if (!utf8 || !buf || bufSize == 0) return 0;
    return ImGui::InputText(utf8, buf, bufSize) ? 1 : 0;
}

__declspec(dllexport) int PBZFM_CollapsingHeader(const char* utf8) {
    if (!utf8) return 0;
    return ImGui::CollapsingHeader(utf8) ? 1 : 0;
}

__declspec(dllexport) int PBZFM_Selectable(const char* utf8, int selected) {
    if (!utf8) return 0;
    return ImGui::Selectable(utf8, selected != 0) ? 1 : 0;
}

__declspec(dllexport) void PBZFM_ProgressBar(float fraction, float width, const char* overlay_utf8) {
    ImGui::ProgressBar(fraction, ImVec2(width > 0.f ? width : 0.f, 0.f), overlay_utf8);
}

__declspec(dllexport) int PBZFM_ColorEdit3(const char* utf8, float rgb[3]) {
    if (!utf8 || !rgb) return 0;
    return ImGui::ColorEdit3(utf8, rgb) ? 1 : 0;
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

// ---- textures (v2) ----

__declspec(dllexport) void* PBZFM_LoadTexture(const char* path_utf8) {
    return TexturesLoad(path_utf8);
}

__declspec(dllexport) void PBZFM_DisposeTexture(void* texture) {
    TexturesDispose(texture);
}

__declspec(dllexport) void PBZFM_Image(void* texture, float w, float h) {
    TexturesDraw(texture, w, h);   // draws nothing until the upload lands
}

__declspec(dllexport) void* PBZFM_GetImGuiContext(void) {
    return ImGui::GetCurrentContext();
}

// ---- framework info ----

__declspec(dllexport) const char* PBZFM_Version(void) {
    return pbzfm::kVersion;
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
