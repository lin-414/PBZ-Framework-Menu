#include "schema.h"

#include "common.h"
#include "i18n.h"
#include "config.h"
#include "registry.h"

#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>

namespace pbzfm {

namespace {
std::mutex g_cfgMtx;
std::vector<std::unique_ptr<ConfigDef>> g_configs;

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

bool WriteFileText(const std::wstring& path, const std::string& utf8) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(utf8.data(), 1, utf8.size(), f) == utf8.size();
    fclose(f);
    return ok;
}

// Flat typed JSON: {"key": true|123|1.5|"text", ...}. Values are kept as raw
// strings and coerced by the schema item types on load.
bool ParseFlatTypedJson(const std::string& utf8, std::map<std::string, std::string>& raw) {
    size_t i = 0;
    const size_t n = utf8.size();
    auto skipWs = [&](size_t& p) { while (p < n && (utf8[p] == ' ' || utf8[p] == '\t' || utf8[p] == '\r' || utf8[p] == '\n')) ++p; };
    auto parseString = [&](size_t& p, std::string& str) -> bool {
        skipWs(p);
        if (p >= n || utf8[p] != '"') return false;
        ++p;
        str.clear();
        while (p < n) {
            char c = utf8[p++];
            if (c == '"') return true;
            if (c == '\\' && p < n) {
                char e = utf8[p++];
                switch (e) {
                    case '"': str += '"'; break;
                    case '\\': str += '\\'; break;
                    case '/': str += '/'; break;
                    case 'n': str += '\n'; break;
                    case 't': str += '\t'; break;
                    default: str += e; break;
                }
            } else str += c;
        }
        return false;
    };
    auto parseRawValue = [&](size_t& p, std::string& str) -> bool {
        skipWs(p);
        size_t start = p;
        while (p < n && utf8[p] != ',' && utf8[p] != '}') ++p;
        str = utf8.substr(start, p - start);
        // trim
        size_t a = str.find_first_not_of(" \t\r\n");
        size_t b = str.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return false;
        str = str.substr(a, b - a + 1);
        return true;
    };
    skipWs(i);
    if (i >= n || utf8[i] != '{') return false;
    ++i;
    while (true) {
        skipWs(i);
        if (i < n && utf8[i] == '}') return true;
        std::string key, val;
        if (!parseString(i, key)) return false;
        skipWs(i);
        if (i >= n || utf8[i] != ':') return false;
        ++i;
        if (i < n && utf8[i] == '"') {
            if (!parseString(i, val)) return false;
        } else {
            if (!parseRawValue(i, val)) return false;
        }
        raw[key] = val;
        skipWs(i);
        if (i < n && utf8[i] == ',') { ++i; continue; }
        if (i < n && utf8[i] == '}') return true;
        return false;
    }
}

} // namespace

SchemaItem* ConfigDef::Find(const char* key) {
    if (!key) return nullptr;
    for (auto& it : items) {
        if (it.key == key) return &it;
    }
    return nullptr;
}

bool ConfigDef::Load(const std::wstring& path) {
    std::string text = ReadFileText(path);
    if (text.empty()) return false;
    std::map<std::string, std::string> raw;
    if (!ParseFlatTypedJson(text, raw)) {
        Log("[schema] store %s: unparsable json, ignored", store.c_str());
        return false;
    }
    for (auto& it : items) {
        auto found = raw.find(it.key);
        if (found == raw.end()) continue;
        const std::string& v = found->second;
        switch (it.type) {
            case SCH_BOOL:
                it.ival = (v == "true" || v == "1") ? 1 : 0;
                break;
            case SCH_INT:
            case SCH_ENUM:
            case SCH_KEY:
                it.ival = strtoll(v.c_str(), nullptr, 10);
                break;
            case SCH_FLOAT:
                it.fval = (float)strtod(v.c_str(), nullptr);
                break;
            default:
                break;
        }
    }
    // clamp
    for (auto& it : items) {
        switch (it.type) {
            case SCH_INT:
                it.ival = std::clamp<long long>(it.ival, (long long)it.mn, (long long)it.mx);
                break;
            case SCH_ENUM:
                it.ival = std::clamp<long long>(it.ival, 0, (long long)it.options.size() - 1);
                break;
            case SCH_FLOAT:
                it.fval = std::clamp(it.fval, it.mn, it.mx);
                break;
            default:
                break;
        }
    }
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
        switch (it.type) {
            case SCH_BOOL:
                out += "  \"" + it.key + "\": " + std::string(it.ival ? "true" : "false");
                break;
            case SCH_INT:
            case SCH_ENUM:
            case SCH_KEY:
                snprintf(buf, sizeof(buf), "  \"%s\": %lld", it.key.c_str(), it.ival);
                out += buf;
                break;
            case SCH_FLOAT:
                snprintf(buf, sizeof(buf), "  \"%s\": %.6g", it.key.c_str(), it.fval);
                out += buf;
                break;
            default:
                break;
        }
    }
    out += "\n}\n";
    bool ok = WriteFileText(path, out);
    if (ok) dirty = false;
    return ok;
}

ConfigDef* SchemaCreateConfig(const char* store_utf8, const char* page_label_utf8) {
    ConfigDef* cfg = new ConfigDef();
    cfg->store = store_utf8 ? store_utf8 : "config";
    Section* s = RegistryCurrentSection();
    if (!s) {
        Log("[schema] no current section for config %s (call SetSection first)", cfg->store.c_str());
        delete cfg;
        return nullptr;
    }
    Page p;
    p.label = page_label_utf8 ? page_label_utf8 : cfg->store;
    p.schemaCfg = cfg;
    RegistryAddPage(s, p);
    {
        std::lock_guard<std::mutex> lock(g_cfgMtx);
        g_configs.emplace_back(cfg);
    }
    if (cfg->Load(StorePath(cfg->store))) {
        Log("[schema] store loaded: %s", cfg->store.c_str());
    }
    return cfg;
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
    if (cfg->Save(StorePath(cfg->store))) {
        Log("[schema] saved: %s", cfg->store.c_str());
    }
}

void SchemaSaveAllDirty() {
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
float g_lastItemRect[4] = { -1, -1, -1, -1 };

bool SchemaGetLastItemRect(float out_xyxy[4]) {
    if (!out_xyxy) return false;
    if (g_lastItemRect[0] < 0) return false;
    for (int i = 0; i < 4; ++i) out_xyxy[i] = g_lastItemRect[i];
    return true;
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
                ImGui::TextUnformatted(i18n.T(it.label.c_str()));
                ImGui::SameLine(0.0f, 12.0f);
                char kb[64];
                snprintf(kb, sizeof(kb), "[%s]", Config::VkName((unsigned)it.ival));
                bool capturing = g_captureMode.load(std::memory_order_relaxed) == 1 &&
                                 g_captureKind.load(std::memory_order_relaxed) == CAP_SCHEMA_KEY &&
                                 g_capCfg == (void*)&cfg && g_capItem == (void*)&it;
                if (capturing) {
                    ImGui::Button(i18n.T("capturing"), ImVec2(150.0f, 0.0f));
                } else if (ImGui::Button(kb, ImVec2(150.0f, 0.0f))) {
                    g_capCfg = (void*)&cfg;
                    g_capItem = (void*)&it;
                    g_captureKind.store(CAP_SCHEMA_KEY, std::memory_order_relaxed);
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
