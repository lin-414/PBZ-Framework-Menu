#include "i18n.h"
#include "common.h"
#include <cstdio>
#include <cstdlib>

namespace pbzfm {

static const char* kJsonZh =
R"json({
  "title": "PBZ-Framework Menu",
  "framework_section": "PBZ-Framework Menu",
  "page_settings": "设置",
  "hint": "DirectComposition 独立覆盖窗",
  "btn_test": "测试按钮",
  "count": "计数",
  "sec_display": "显示",
  "sec_demo": "演示参数",
  "sec_keys": "键位",
  "language": "语言",
  "ui_scale": "界面缩放",
  "ui_opacity": "不透明度",
  "fov": "视野 (FOV)",
  "freeze": "时间冻结",
  "quality": "画质预设",
  "q_low": "低",
  "q_medium": "中",
  "q_high": "高",
  "q_custom": "自定义",
  "toggle_key": "打开/关闭菜单",
  "capturing": "按下新按键…",
  "save": "保存配置",
  "close_menu": "关闭菜单 (Esc)",
  "saved": "已保存到 PBZFM.ini"
})json";

static const char* kJsonEn =
R"json({
  "title": "PBZ-Framework Menu",
  "framework_section": "PBZ-Framework Menu",
  "page_settings": "Settings",
  "hint": "Independent DirectComposition overlay",
  "btn_test": "Test button",
  "count": "clicks",
  "sec_display": "Display",
  "sec_demo": "Demo settings",
  "sec_keys": "Key bindings",
  "language": "Language",
  "ui_scale": "UI scale",
  "ui_opacity": "Opacity",
  "fov": "Field of view (FOV)",
  "freeze": "Freeze time",
  "quality": "Quality preset",
  "q_low": "Low",
  "q_medium": "Medium",
  "q_high": "High",
  "q_custom": "Custom",
  "toggle_key": "Toggle menu",
  "capturing": "Press a new key...",
  "save": "Save settings",
  "close_menu": "Close menu (Esc)",
  "saved": "Saved to PBZFM.ini"
})json";

static std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Flat "key": "value" object parser (good enough for the shipped tables).
static bool ParseFlatJson(const std::string& utf8, std::map<std::string, std::string>& out) {
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
                    case 'r': str += '\r'; break;
                    case 'b': str += '\b'; break;
                    case 'f': str += '\f'; break;
                    case 'u': {
                        if (p + 4 > n) return false;
                        unsigned cp = (unsigned)strtoul(utf8.substr(p, 4).c_str(), nullptr, 16);
                        p += 4;
                        // BMP-only UTF-8 encode; our tables never use surrogates.
                        if (cp < 0x80) str += (char)cp;
                        else if (cp < 0x800) { str += (char)(0xC0 | (cp >> 6)); str += (char)(0x80 | (cp & 0x3F)); }
                        else { str += (char)(0xE0 | (cp >> 12)); str += (char)(0x80 | ((cp >> 6) & 0x3F)); str += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: return false;
                }
            } else str += c;
        }
        return false;
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
        if (!parseString(i, val)) return false;
        out[key] = val;
        skipWs(i);
        if (i < n && utf8[i] == ',') { ++i; continue; }
        if (i < n && utf8[i] == '}') return true;
        return false;
    }
}

static std::string ReadFileText(const std::wstring& path) {
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

static bool LoadTable(const std::wstring& path, const char* embedded, std::map<std::string, std::string>& out) {
    std::string text = ReadFileText(path);
    if (text.empty() && embedded) text = embedded;
    if (text.empty()) return false;
    std::map<std::string, std::string> parsed;
    if (!ParseFlatJson(text, parsed) && embedded) {
        if (!ParseFlatJson(embedded, parsed)) return false;
    }
    out.swap(parsed);
    return !out.empty();
}

bool I18n::Resolve(const std::string& lang) {
    const char* embedded = (lang == "en-US") ? kJsonEn : kJsonZh;
    std::map<std::string, std::string> table;
    if (!LoadTable(languagesDir_ + L"\\" + Utf8ToWide(lang), embedded, table)) return false;
    strings_.swap(table);
    current_ = lang;
    return true;
}

bool I18n::Load(const std::wstring& languagesDir, const std::wstring& lang) {
    languagesDir_ = languagesDir;
    LoadTable(languagesDir + L"\\en-US.json", kJsonEn, fallback_);
    std::string want = WideToUtf8(lang);
    if (!Resolve(want)) {
        LoadTable(languagesDir + L"\\zh-CN.json", kJsonZh, strings_);
        current_ = "zh-CN";
    }
    Log("[i18n] language=%s, entries=%zu (en fallback=%zu)",
        current_.c_str(), strings_.size(), fallback_.size());
    return true;
}

bool I18n::SetLanguage(const char* utf8Lang) { return Resolve(utf8Lang); }

const char* I18n::T(const char* key) const {
    auto it = strings_.find(key);
    if (it != strings_.end()) return it->second.c_str();
    auto fb = fallback_.find(key);
    if (fb != fallback_.end()) return fb->second.c_str();
    return key;
}

const char* const* I18n::KnownLanguages(int* count) {
    static const char* const langs[] = { "zh-CN", "en-US" };
    if (count) *count = 2;
    return langs;
}

} // namespace pbzfm
