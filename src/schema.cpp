#include "schema.h"

#include "common.h"
#include "flatjson.h"
#include "i18n.h"
#include "config.h"
#include "registry.h"
#include "theme.h"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

namespace pbzfm {

namespace {
std::mutex g_cfgMtx;
std::vector<std::unique_ptr<ConfigDef>> g_configs;

// ImGui 1.92 scales glyphs via style.FontScaleMain but leaves explicit ImVec2
// sizes untouched. A fixed 150px key-cap button therefore clips long key names
// as soon as the UI scale is raised. Keep the old footprint as the scaled
// minimum and reserve enough room for the label itself.
float KeyButtonWidth(const char* label) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float fontScale = st.FontSizeBase > 0.0f ? ImGui::GetFontSize() / st.FontSizeBase : 1.0f;
    const float minWidth = 150.0f * fontScale;
    const float textWidth = ImGui::CalcTextSize(label).x + st.FramePadding.x * 2.0f + 2.0f * fontScale;
    return std::max(minWidth, textWidth);
}

std::wstring Utf8ToWide(const std::string& u);

std::wstring DataDir() { return RootFile(L"data"); }
std::wstring StorePath(const std::string& store) {
    return DataDir() + L"\\" + Utf8ToWide(store) + L".json";
}
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
}

std::string ReadFileText(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    std::string out;
    char buf[4096];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, rd);
    fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return out;
}

// M1: stores go through WriteFileAtomic (common.h) so a torn write can never
// leave a half-written json behind; the local FILE*-based writer is gone.

// M2: keys were emitted verbatim, so a key containing a quote, backslash or
// control character made the whole store unparsable on the next load. Escape
// the minimal JSON set; ParseFlatJsonRaw already decodes these escapes, and
// ordinary keys keep their exact previous bytes (old stores stay compatible).
std::string JsonEscapeKey(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)c);
                    out += esc;
                } else {
                    out += (char)c;
                }
                break;
        }
    }
    return out;
}

} // namespace

SchemaItem* ConfigDef::Find(const char* key) {
    if (!key) return nullptr;
    for (auto& it : items) {
        if (it.key == key) return &it;
    }
    return nullptr;
}

void ConfigDef::ApplyStored(SchemaItem& it) const {
    // B1: two-phase persistence. At creation (SchemaCreateConfigAt) Load() ran
    // against an empty items list; ConfigAdd*() calls this as each control
    // appears so the value already on disk wins over the declared default.
    // A missing key or a non-numeric token keeps the default (old behavior).
    if (it.type == SCH_GROUP || it.key.empty()) return;
    auto found = stored.find(it.key);
    if (found == stored.end()) return;
    const std::string& v = found->second;
    char* end = nullptr;
    switch (it.type) {
        case SCH_BOOL:
            it.ival = (v == "true" || v == "1") ? 1 : 0;
            break;
        case SCH_INT:
        case SCH_ENUM:
        case SCH_KEY:
        case SCH_KEYCOMBO:   // packed vk | (mods << 8)
            it.ival = strtoll(v.c_str(), &end, 10);
            if (end == v.c_str()) return;   // non-numeric disk token: keep the default
            // M6: an old store may hold the Win modifier, which can never fire
            // (the hook must let Win reach the shell). Drop that bit so the item
            // shows and matches the working part of the chord instead of a dead
            // "Win+" label; the next save persists the normalized value.
            if (it.ival & 0x800) {
                Log("[schema] %s: Win modifier in '%s' is unsupported, dropped",
                    store.c_str(), it.key.c_str());
                it.ival &= ~0x800;
            }
            break;
        case SCH_FLOAT:
            it.fval = (float)strtod(v.c_str(), &end);
            if (end == v.c_str()) return;
            break;
        default:
            return;
    }
    // Same clamping Load() applied after coercion; reused here so an item
    // declared late cannot end up outside its slider/combo range.
    switch (it.type) {
        case SCH_INT:
            it.ival = std::clamp<long long>(it.ival, (long long)it.mn, (long long)it.mx);
            break;
        case SCH_ENUM:
            it.ival = it.options.empty() ? 0 : std::clamp<long long>(it.ival, 0, (long long)it.options.size() - 1);
            break;
        case SCH_FLOAT:
            it.fval = std::clamp(it.fval, it.mn, it.mx);
            break;
        default:
            break;
    }
}

bool ConfigDef::Load(const std::wstring& path) {
    std::string text = ReadFileText(path);
    if (text.empty()) return false;
    std::map<std::string, std::string> raw;
    if (!ParseFlatJsonRaw(text, raw)) {
        Log("[schema] store %s: unparsable json, ignored", store.c_str());
        return false;
    }
    // B1: keep the raw key -> token map; items declared later pull their
    // value from it through ApplyStored(). An explicit reload arrives with
    // items already present, so apply the map to them as well. Duplicate
    // keys behave as before (the later item simply reads the same token).
    stored = std::move(raw);
    for (auto& it : items) ApplyStored(it);
    dirty = false;
    return true;
}

bool ConfigDef::Save(const std::wstring& path) {
    CreateDirectoryW(DataDir().c_str(), nullptr);
    char buf[256];
    std::string out = "{\n";
    bool first = true;
    for (auto& it : items) {
        if (it.type == SCH_GROUP) continue;
        if (!first) out += ",\n";
        first = false;
        // M2: build the key through the escaper; the value text stays numeric.
        const std::string key = JsonEscapeKey(it.key);
        switch (it.type) {
            case SCH_BOOL:
                out += "  \"" + key + "\": " + std::string(it.ival ? "true" : "false");
                break;
            case SCH_INT:
            case SCH_ENUM:
            case SCH_KEY:
            case SCH_KEYCOMBO:
                out += "  \"" + key + "\": " + std::to_string(it.ival);
                break;
            case SCH_FLOAT:
                snprintf(buf, sizeof(buf), "%.6g", it.fval);
                out += "  \"" + key + "\": " + buf;
                break;
            default:
                break;
        }
    }
    out += "\n}\n";
    bool ok = WriteFileAtomic(path, out);   // M1: atomic replace, failure already logged
    if (ok) dirty = false;
    return ok;
}

ConfigDef* SchemaCreateConfigAt(Section* section, const char* store_utf8, const char* page_label_utf8) {
    if (!section) return nullptr;
    // H8: the Section* is only guaranteed alive while the registry lock is held
    // (a concurrent DeleteSection frees the node). Callers such as the nested
    // path API already wrap this in RegistryWithPath; taking the recursive lock
    // here makes the guarantee local to the function instead of caller folklore.
    std::lock_guard<std::recursive_mutex> registryLock(RegistryMutex());
    ConfigDef* cfg = new ConfigDef();
    cfg->store = store_utf8 ? store_utf8 : "config";
    Page p;
    p.label = page_label_utf8 ? page_label_utf8 : cfg->store;
    p.schemaCfg = cfg;
    RegistryAddPage(section, p);
    {
        std::lock_guard<std::mutex> lock(g_cfgMtx);
        g_configs.emplace_back(cfg);
    }
    // H3: Load() applies the disk snapshot to `items`; it runs under the same
    // registry lock that guards every items mutation, so a render frame can
    // never observe a half-applied list.
    if (cfg->Load(StorePath(cfg->store))) {
        Log("[schema] store loaded: %s", cfg->store.c_str());
    }
    return cfg;
}

ConfigDef* SchemaCreateConfig(const char* store_utf8, const char* page_label_utf8) {
    // H8: resolve the current section and create the page in one registry
    // critical section - the node cannot disappear in between.
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    Section* s = RegistryCurrentSection();
    if (!s) {
        Log("[schema] no current section for config %s (call SetSection first)",
            store_utf8 ? store_utf8 : "config");
        return nullptr;
    }
    return SchemaCreateConfigAt(s, store_utf8, page_label_utf8);
}

ConfigDef* SchemaValidate(void* handle) {
    if (!handle) return nullptr;
    ConfigDef* c = (ConfigDef*)handle;
    std::lock_guard<std::mutex> lock(g_cfgMtx);
    for (auto& cfg : g_configs) {
        if (cfg.get() == c && c->magic == 0x50424643) return c;
    }
    return nullptr;
}

void SchemaSaveConfigNow(ConfigDef* cfg) {
    if (!cfg) return;
    // H3: Save walks ConfigDef::items, which api.cpp mutates under the registry
    // lock (ConfigAdd*). Taking the same recursive lock here keeps a render-thread
    // save (key capture, menu button) from racing a mod thread's declaration.
    // Lock order stays R -> C, the only order any path uses.
    std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
    if (cfg->Save(StorePath(cfg->store))) {
        Log("[schema] saved: %s", cfg->store.c_str());
    }
}

void SchemaSaveAllDirty() {
    // H3: same lock as SchemaSaveConfigNow; it also guards the items walk and
    // runs on the render thread at menu close, outside the frame's own lock.
    std::lock_guard<std::recursive_mutex> registryLock(RegistryMutex());
    std::lock_guard<std::mutex> lock(g_cfgMtx);
    for (auto& cfg : g_configs) {
        if (cfg->dirty) {
            if (cfg->Save(StorePath(cfg->store))) {
                Log("[schema] saved dirty store: %s", cfg->store.c_str());
            }
        }
    }
}

// Rect of the most recently rendered schema item (automation aid).
// L4: the probe thread reads it while the render thread writes it, and a rect
// left over from the previous page would be reported as the current page's item;
// the mutex keeps the 4-float snapshot consistent, the clear runs on page switch.
std::mutex g_itemRectMtx;
float g_lastItemRect[4] = { -1, -1, -1, -1 };

bool SchemaGetLastItemRect(float out_xyxy[4]) {
    if (!out_xyxy) return false;
    std::lock_guard<std::mutex> lock(g_itemRectMtx);
    if (g_lastItemRect[0] < 0) return false;
    for (int i = 0; i < 4; ++i) out_xyxy[i] = g_lastItemRect[i];
    return true;
}

void SchemaClearLastItemRect() {
    std::lock_guard<std::mutex> lock(g_itemRectMtx);
    g_lastItemRect[0] = g_lastItemRect[1] = g_lastItemRect[2] = g_lastItemRect[3] = -1;
}

void RenderSchemaForm(ConfigDef& cfg, const I18n& i18n, float dt) {
    (void)dt;
    bool firstRectRecorded = false;
    for (auto& it : cfg.items) {
        switch (it.type) {
            case SCH_GROUP:
                ImGui::SeparatorText(i18n.T(it.label.c_str()));
                break;
            case SCH_BOOL: {
                bool b = it.ival != 0;
                if (ImGui::Checkbox(i18n.T(it.label.c_str()), &b)) {
                    it.ival = b ? 1 : 0;
                    cfg.dirty = true;
                    cfg.FireChange(it.key.c_str());
                }
                break;
            }
            case SCH_INT: {
                int v = (int)it.ival;
                if (ImGui::SliderInt(i18n.T(it.label.c_str()), &v, (int)it.mn, (int)it.mx)) {
                    it.ival = v;
                    cfg.dirty = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) cfg.FireChange(it.key.c_str());
                break;
            }
            case SCH_FLOAT: {
                float v = it.fval;
                if (ImGui::SliderFloat(i18n.T(it.label.c_str()), &v, it.mn, it.mx, it.fmt.c_str())) {
                    it.fval = v;
                    cfg.dirty = true;
                }
                if (ImGui::IsItemDeactivatedAfterEdit()) cfg.FireChange(it.key.c_str());
                break;
            }
            case SCH_ENUM: {
                int v = (int)it.ival;
                std::vector<const char*> opts;
                for (auto& o : it.options) opts.push_back(o.c_str());
                if (ImGui::Combo(i18n.T(it.label.c_str()), &v, opts.data(), (int)opts.size())) {
                    it.ival = v;
                    cfg.dirty = true;
                    cfg.FireChange(it.key.c_str());
                }
                break;
            }
            case SCH_KEY: {
                const char* label = i18n.T(it.label.c_str());
                char kb[64];
                snprintf(kb, sizeof(kb), "[%s]", Config::VkName((unsigned)it.ival));
                bool capturing = g_captureMode.load(std::memory_order_relaxed) == 1 &&
                                 g_captureKind.load(std::memory_order_relaxed) == CAP_SCHEMA_KEY &&
                                 g_capCfg == (void*)&cfg && g_capItem == (void*)&it;
                const char* buttonLabel = capturing ? i18n.T("capturing") : kb;
                const float availW = ImGui::GetContentRegionAvail().x;
                float buttonW = KeyButtonWidth(buttonLabel);
                const float fontScale = ImGui::GetStyle().FontSizeBase > 0.0f
                                            ? ImGui::GetFontSize() / ImGui::GetStyle().FontSizeBase
                                            : 1.0f;
                const float gap = 12.0f * fontScale;
                const bool sameLine = ImGui::CalcTextSize(label).x + gap + buttonW <= availW;
                if (!sameLine) buttonW = std::min(buttonW, std::max(1.0f, availW));
                ImGui::TextUnformatted(label);
                if (sameLine) ImGui::SameLine(0.0f, gap);
                if (capturing) {
                    theme::PushCapturingStyle();
                    ImGui::Button(buttonLabel, ImVec2(buttonW, 0.0f));
                    theme::PopCapturingStyle();
                } else if (ImGui::Button(buttonLabel, ImVec2(buttonW, 0.0f))) {
                    g_capCfg = (void*)&cfg;
                    g_capItem = (void*)&it;
                    g_captureKind.store(CAP_SCHEMA_KEY, std::memory_order_relaxed);
                    g_captureMode.store(1, std::memory_order_relaxed);
                }
                break;
            }
            case SCH_KEYCOMBO: {
                const char* label = i18n.T(it.label.c_str());
                unsigned vk = (unsigned)it.ival & 0xFFu;
                unsigned mods = ((unsigned)it.ival >> 8) & 0xFu;
                char kb[96];
                snprintf(kb, sizeof(kb), "[%s]", Config::KeyModsToString(vk, mods).c_str());
                bool capturing = g_captureMode.load(std::memory_order_relaxed) == 1 &&
                                 g_captureKind.load(std::memory_order_relaxed) == CAP_SCHEMA_KEYCOMBO &&
                                 g_capCfg == (void*)&cfg && g_capItem == (void*)&it;
                const char* buttonLabel = capturing ? i18n.T("capturing") : kb;
                const float availW = ImGui::GetContentRegionAvail().x;
                float buttonW = KeyButtonWidth(buttonLabel);
                const float fontScale = ImGui::GetStyle().FontSizeBase > 0.0f
                                            ? ImGui::GetFontSize() / ImGui::GetStyle().FontSizeBase
                                            : 1.0f;
                const float gap = 12.0f * fontScale;
                const bool sameLine = ImGui::CalcTextSize(label).x + gap + buttonW <= availW;
                if (!sameLine) buttonW = std::min(buttonW, std::max(1.0f, availW));
                ImGui::TextUnformatted(label);
                if (sameLine) ImGui::SameLine(0.0f, gap);
                if (capturing) {
                    theme::PushCapturingStyle();
                    ImGui::Button(buttonLabel, ImVec2(buttonW, 0.0f));
                    theme::PopCapturingStyle();
                } else if (ImGui::Button(buttonLabel, ImVec2(buttonW, 0.0f))) {
                    g_capCfg = (void*)&cfg;
                    g_capItem = (void*)&it;
                    g_captureKind.store(CAP_SCHEMA_KEYCOMBO, std::memory_order_relaxed);
                    g_captureMode.store(1, std::memory_order_relaxed);
                }
                break;
            }
            default:
                break;
        }
        ImVec2 ra = ImGui::GetItemRectMin();
        ImVec2 rb = ImGui::GetItemRectMax();
        if (!firstRectRecorded && it.type != SCH_GROUP) {
            firstRectRecorded = true;
            std::lock_guard<std::mutex> lock(g_itemRectMtx);
            g_lastItemRect[0] = ra.x;
            g_lastItemRect[1] = ra.y;
            g_lastItemRect[2] = rb.x;
            g_lastItemRect[3] = rb.y;
        }
    }
}

void* g_capCfg = nullptr;
void* g_capItem = nullptr;

} // namespace pbzfm