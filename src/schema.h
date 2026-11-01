#pragma once
// Schema-backed settings: mods declare typed items, the framework renders a
// form for them, persists values per store (data/<store>.json) and fires the
// mod's change callback on every committed edit.
#include <string>
#include <vector>

#include "pbzfm.h"

namespace pbzfm {

class I18n;
struct MenuUi;
struct Section;

enum SchemaType { SCH_GROUP = 0, SCH_BOOL = 1, SCH_INT = 2, SCH_FLOAT = 3, SCH_ENUM = 4, SCH_KEY = 5,
                  SCH_KEYCOMBO = 6 };   // v2: chord keybind, ival = vk | (mods << 8)

struct SchemaItem {
    int type = SCH_GROUP;
    std::string key, label, fmt;
    long long ival = 0;       // bool / int / enum index / virtual-key
    float fval = 0.f;         // float
    float mn = 0.f, mx = 0.f, step = 1.f;
    std::vector<std::string> options;
};

struct ConfigDef {
    unsigned magic = 0x50424643;  // "PBFC"
    std::string store;
    std::vector<SchemaItem> items;
    PBZFM_ChangeFn changeFn = nullptr;
    void* changeUd = nullptr;
    bool dirty = false;

    SchemaItem* Find(const char* key);
    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path);
    void FireChange(const char* key) {
        if (changeFn) changeFn(key, changeUd);
    }
};

// Creates a config + its auto-rendered page in the calling thread's current
// section. Loads data/<store>.json when present.
ConfigDef* SchemaCreateConfig(const char* store_utf8, const char* page_label_utf8);
// Same, but the page lands on an explicit tree node (the nested-path API).
ConfigDef* SchemaCreateConfigAt(Section* section, const char* store_utf8, const char* page_label_utf8);
ConfigDef* SchemaValidate(void* handle);            // null when the handle is not a live config
void SchemaSaveConfigNow(ConfigDef* cfg);           // immediate file write
void SchemaSaveAllDirty();                          // called on menu close

// ImGui form for a config (call inside the menu frame). Uses the shared key
// capture flow for SCH_KEY items. Commit semantics:
//   BOOL/ENUM/KEY: change fires immediately; INT/FLOAT: on release
//   (IsItemDeactivatedAfterEdit). File writes happen on ConfigSave / menu close.
void RenderSchemaForm(ConfigDef& cfg, const I18n& i18n, float dt);
bool SchemaGetLastItemRect(float out_xyxy[4]);   // overlay-client px, false until a form rendered

// Key capture targets (render thread only; the capture mode flags are the
// atomics in common.h).
enum CaptureKind { CAP_NONE = 0, CAP_TOGGLE = 1, CAP_SCHEMA_KEY = 2, CAP_SCHEMA_KEYCOMBO = 3 };
extern void* g_capCfg;         // ConfigDef* for CAP_SCHEMA_KEY
extern void* g_capItem;        // SchemaItem* target of the active capture

} // namespace pbzfm
