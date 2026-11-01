#include "config.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <algorithm>

namespace pbzfm {

static std::wstring Trim(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n");
    if (a == std::wstring::npos) return L"";
    size_t b = s.find_last_not_of(L" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
}

static std::string ReadFileText(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return out;
}

static bool WriteFileText(const std::wstring& path, const std::string& utf8) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    bool ok = fwrite(utf8.data(), 1, utf8.size(), f) == utf8.size();
    fclose(f);
    return ok;
}

bool Config::Load(const std::wstring& path) {
    std::string text = ReadFileText(path);
    if (text.empty()) return false;
    std::wstring wide = Utf8ToWide(text);
    std::wstring section;
    size_t pos = 0;
    while (pos <= wide.size()) {
        size_t eol = wide.find(L'\n', pos);
        if (eol == std::wstring::npos) eol = wide.size();
        std::wstring line = Trim(wide.substr(pos, eol - pos));
        pos = eol + 1;
        if (line.empty() || line[0] == L';' || line[0] == L'#') continue;
        if (line.front() == L'[' && line.back() == L']') { section = Trim(line.substr(1, line.size() - 2)); continue; }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        std::wstring key = Trim(line.substr(0, eq));
        std::wstring val = Trim(line.substr(eq + 1));
        if (key.empty()) continue;
        std::string k = WideToUtf8(key);
        if (section == L"framework") {
            if (k == "language") language = WideToUtf8(val);
            else if (k == "toggle_key") { UINT vk = VkFromName(val); if (vk) toggleVk = vk; }
            else if (k == "passthrough_keys") passthroughKeys = ChordsFromText(val);
            else if (k == "passthrough_replay") passthroughReplay = (val == L"1" || _wcsicmp(val.c_str(), L"true") == 0);
        } else if (section == L"ui") {
            if (k == "ui_scale") uiScale = (float)_wtof(val.c_str());
            else if (k == "ui_opacity") uiOpacity = (float)_wtof(val.c_str());
            else if (k == "theme") theme = WideToUtf8(val);
        } else if (section == L"demo") {
            if (k == "fov") fov = (float)_wtof(val.c_str());
            else if (k == "freeze") freeze = (val == L"1" || _wcsicmp(val.c_str(), L"true") == 0);
            else if (k == "quality") quality = _wtoi(val.c_str());
            else if (k == "demo_clicks") clicks = wcstol(val.c_str(), nullptr, 10);
        }
    }
    uiScale = std::clamp(uiScale, 0.7f, 1.8f);
    uiOpacity = std::clamp(uiOpacity, 0.4f, 1.0f);
    quality = std::clamp(quality, 0, 3);
    return true;
}

bool Config::Save(const std::wstring& path) const {
    char buf[1024];
    std::string out;
    out += "; PBZ Menu Framework prototype config\n";
    out += "\n[framework]\n";
    out += "language = " + language + "\n";
    snprintf(buf, sizeof(buf), "toggle_key = %s\n", VkName(toggleVk));
    out += buf;
    std::string chords;
    for (size_t i = 0; i < passthroughKeys.size(); ++i) {
        if (i) chords += ", ";
        chords += ChordToString(passthroughKeys[i]);
    }
    out += "passthrough_keys = " + chords + "\n";
    snprintf(buf, sizeof(buf), "passthrough_replay = %s\n", passthroughReplay ? "true" : "false");
    out += buf;
    out += "\n[ui]\n";
    snprintf(buf, sizeof(buf), "ui_scale = %.2f\nui_opacity = %.2f\n", uiScale, uiOpacity);
    out += buf;
    out += "theme = " + theme + "\n";
    out += "\n[demo]\n";
    snprintf(buf, sizeof(buf), "fov = %.1f\nfreeze = %s\nquality = %d\ndemo_clicks = %ld\n",
             fov, freeze ? "true" : "false", quality, clicks);
    out += buf;
    return WriteFileText(path, out);
}

// "Alt+P, Alt+F1" -> two chords. Tokens are matched case-insensitively; an
// unknown key name or a second non-modifier token drops the whole chord.
std::vector<HotkeyChord> Config::ChordsFromText(const std::wstring& csv) {
    std::vector<HotkeyChord> out;
    size_t pos = 0;
    while (pos <= csv.size()) {
        size_t comma = csv.find(L',', pos);
        if (comma == std::wstring::npos) comma = csv.size();
        std::wstring item = Trim(csv.substr(pos, comma - pos));
        pos = comma + 1;
        if (item.empty()) continue;
        HotkeyChord c{};
        bool haveVk = false, valid = true;
        size_t start = 0;
        while (start <= item.size()) {
            size_t plus = item.find(L'+', start);
            std::wstring tok = Trim(item.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
            if (_wcsicmp(tok.c_str(), L"ctrl") == 0 || _wcsicmp(tok.c_str(), L"control") == 0) c.ctrl = true;
            else if (_wcsicmp(tok.c_str(), L"shift") == 0) c.shift = true;
            else if (_wcsicmp(tok.c_str(), L"alt") == 0) c.alt = true;
            else if (_wcsicmp(tok.c_str(), L"win") == 0) c.win = true;
            else if (!haveVk) {
                c.vk = VkFromName(tok);
                haveVk = true;
                if (!c.vk) { valid = false; break; }
            } else { valid = false; break; }
            if (plus == std::wstring::npos) break;
            start = plus + 1;
        }
        if (valid && haveVk && c.vk) out.push_back(c);
    }
    return out;
}

std::string Config::ChordToString(const HotkeyChord& c) {
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.shift) s += "Shift+";
    if (c.alt) s += "Alt+";
    if (c.win) s += "Win+";
    s += VkName(c.vk);
    return s;
}

std::string Config::KeyModsToString(unsigned vk, unsigned mods) {
    HotkeyChord c{};
    c.vk = vk;
    c.ctrl = (mods & 0x1u) != 0;
    c.shift = (mods & 0x2u) != 0;
    c.alt = (mods & 0x4u) != 0;
    c.win = (mods & 0x8u) != 0;
    return ChordToString(c);
}

const char* Config::VkName(UINT vk) {
    static char name[16];
    if (vk >= '0' && vk <= '9' || vk >= 'A' && vk <= 'Z') { name[0] = (char)vk; name[1] = 0; return name; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) { snprintf(name, sizeof(name), "NUMPAD%d", (int)(vk - VK_NUMPAD0)); return name; }
    if (vk >= VK_F1 && vk <= VK_F24) { snprintf(name, sizeof(name), "F%d", (int)(vk - VK_F1 + 1)); return name; }
    switch (vk) {
        case VK_BACK: return "BACKSPACE"; case VK_TAB: return "TAB"; case VK_RETURN: return "ENTER";
        case VK_CAPITAL: return "CAPSLOCK"; case VK_ESCAPE: return "ESC"; case VK_SPACE: return "SPACE";
        case VK_PRIOR: return "PAGEUP"; case VK_NEXT: return "PAGEDOWN"; case VK_END: return "END";
        case VK_HOME: return "HOME"; case VK_LEFT: return "LEFT"; case VK_UP: return "UP";
        case VK_RIGHT: return "RIGHT"; case VK_DOWN: return "DOWN"; case VK_INSERT: return "INSERT";
        case VK_DELETE: return "DELETE"; case VK_MULTIPLY: return "MULTIPLY"; case VK_ADD: return "ADD";
        case VK_SUBTRACT: return "SUBTRACT"; case VK_DECIMAL: return "DECIMAL"; case VK_DIVIDE: return "DIVIDE";
        case VK_OEM_1: return "SEMICOLON"; case VK_OEM_PLUS: return "EQUAL"; case VK_OEM_COMMA: return "COMMA";
        case VK_OEM_MINUS: return "MINUS"; case VK_OEM_PERIOD: return "PERIOD"; case VK_OEM_2: return "SLASH";
        case VK_OEM_3: return "GRAVE"; case VK_OEM_4: return "LBRACKET"; case VK_OEM_5: return "BACKSLASH";
        case VK_OEM_6: return "RBRACKET"; case VK_OEM_7: return "APOSTROPHE";
        default: snprintf(name, sizeof(name), "0x%02X", vk); return name;
    }
}

UINT Config::VkFromName(const std::wstring& n) {
    std::wstring s = n;
    std::transform(s.begin(), s.end(), s.begin(), ::towupper);
    if (s.empty()) return 0;
    if (s.size() == 1) {
        wchar_t c = s[0];
        if (c >= L'0' && c <= L'9') return (UINT)(c - L'0' + '0');
        if (c >= L'A' && c <= L'Z') return (UINT)(c - L'A' + 'A');
    }
    if (s.rfind(L"F", 0) == 0 && s.size() >= 2) {
        int f = _wtoi(s.c_str() + 1);
        if (f >= 1 && f <= 24) return (UINT)(VK_F1 + f - 1);
    }
    if (s.rfind(L"NUMPAD", 0) == 0 && s.size() == 7) {
        wchar_t c = s[6];
        if (c >= L'0' && c <= L'9') return (UINT)(VK_NUMPAD0 + (c - L'0'));
    }
    static const struct { const wchar_t* name; UINT vk; } table[] = {
        {L"BACKSPACE", VK_BACK}, {L"TAB", VK_TAB}, {L"ENTER", VK_RETURN}, {L"CAPSLOCK", VK_CAPITAL},
        {L"ESC", VK_ESCAPE}, {L"SPACE", VK_SPACE}, {L"PAGEUP", VK_PRIOR}, {L"PAGEDOWN", VK_NEXT},
        {L"END", VK_END}, {L"HOME", VK_HOME}, {L"LEFT", VK_LEFT}, {L"UP", VK_UP}, {L"RIGHT", VK_RIGHT},
        {L"DOWN", VK_DOWN}, {L"INSERT", VK_INSERT}, {L"DELETE", VK_DELETE}, {L"MULTIPLY", VK_MULTIPLY},
        {L"ADD", VK_ADD}, {L"SUBTRACT", VK_SUBTRACT}, {L"DECIMAL", VK_DECIMAL}, {L"DIVIDE", VK_DIVIDE},
        {L"SEMICOLON", VK_OEM_1}, {L"EQUAL", VK_OEM_PLUS}, {L"COMMA", VK_OEM_COMMA}, {L"MINUS", VK_OEM_MINUS},
        {L"PERIOD", VK_OEM_PERIOD}, {L"SLASH", VK_OEM_2}, {L"GRAVE", VK_OEM_3}, {L"LBRACKET", VK_OEM_4},
        {L"BACKSLASH", VK_OEM_5}, {L"RBRACKET", VK_OEM_6}, {L"APOSTROPHE", VK_OEM_7},
    };
    for (auto& e : table) if (s == e.name) return e.vk;
    if (s.rfind(L"0X", 0) == 0) return (UINT)wcstoul(s.c_str() + 2, nullptr, 16);
    return 0;
}

} // namespace pbzfm
