#include "theme.h"

#include "common.h"
#include "flatjson.h"

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace pbzfm { namespace theme {

namespace {

float s_lastWindowOpacity = 0.94f;

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

// "#RRGGBB" (opaque) or "#RRGGBBAA".
ImVec4 FromHex(const std::string& hex, bool* ok) {
    *ok = false;
    ImVec4 c{ 0, 0, 0, 1 };
    if (hex.size() != 7 && hex.size() != 9) return c;
    if (hex[0] != '#') return c;
    for (size_t i = 1; i < hex.size(); ++i) {
        if (!isxdigit((unsigned char)hex[i])) return c;
    }
    auto ch = [&](size_t off) {
        char b[3] = { hex[off], hex[off + 1], 0 };
        return (float)strtoul(b, nullptr, 16) / 255.0f;
    };
    c.x = ch(1);
    c.y = ch(3);
    c.z = ch(5);
    c.w = hex.size() == 9 ? ch(7) : 1.0f;
    *ok = true;
    return c;
}

struct ColName { const char* name; ImGuiCol col; };
const ColName kColNames[] = {
    {"Text", ImGuiCol_Text}, {"TextDisabled", ImGuiCol_TextDisabled},
    {"WindowBg", ImGuiCol_WindowBg}, {"ChildBg", ImGuiCol_ChildBg}, {"PopupBg", ImGuiCol_PopupBg},
    {"Border", ImGuiCol_Border}, {"BorderShadow", ImGuiCol_BorderShadow},
    {"FrameBg", ImGuiCol_FrameBg}, {"FrameBgHovered", ImGuiCol_FrameBgHovered},
    {"FrameBgActive", ImGuiCol_FrameBgActive},
    {"TitleBg", ImGuiCol_TitleBg}, {"TitleBgActive", ImGuiCol_TitleBgActive},
    {"TitleBgCollapsed", ImGuiCol_TitleBgCollapsed},
    {"MenuBarBg", ImGuiCol_MenuBarBg},
    {"ScrollbarBg", ImGuiCol_ScrollbarBg}, {"ScrollbarGrab", ImGuiCol_ScrollbarGrab},
    {"ScrollbarGrabHovered", ImGuiCol_ScrollbarGrabHovered}, {"ScrollbarGrabActive", ImGuiCol_ScrollbarGrabActive},
    {"CheckMark", ImGuiCol_CheckMark}, {"SliderGrab", ImGuiCol_SliderGrab},
    {"SliderGrabActive", ImGuiCol_SliderGrabActive},
    {"Button", ImGuiCol_Button}, {"ButtonHovered", ImGuiCol_ButtonHovered}, {"ButtonActive", ImGuiCol_ButtonActive},
    {"Header", ImGuiCol_Header}, {"HeaderHovered", ImGuiCol_HeaderHovered}, {"HeaderActive", ImGuiCol_HeaderActive},
    {"Separator", ImGuiCol_Separator}, {"SeparatorHovered", ImGuiCol_SeparatorHovered},
    {"SeparatorActive", ImGuiCol_SeparatorActive},
    {"ResizeGrip", ImGuiCol_ResizeGrip}, {"ResizeGripHovered", ImGuiCol_ResizeGripHovered},
    {"ResizeGripActive", ImGuiCol_ResizeGripActive},
    {"Tab", ImGuiCol_Tab}, {"TabHovered", ImGuiCol_TabHovered}, {"TabSelected", ImGuiCol_TabSelected},
    {"TabDimmed", ImGuiCol_TabDimmed}, {"TabDimmedSelected", ImGuiCol_TabDimmedSelected},
    {"PlotLines", ImGuiCol_PlotLines}, {"PlotLinesHovered", ImGuiCol_PlotLinesHovered},
    {"PlotHistogram", ImGuiCol_PlotHistogram}, {"PlotHistogramHovered", ImGuiCol_PlotHistogramHovered},
    {"TableHeaderBg", ImGuiCol_TableHeaderBg}, {"TableBorderStrong", ImGuiCol_TableBorderStrong},
    {"TableBorderLight", ImGuiCol_TableBorderLight},
    {"TextLink", ImGuiCol_TextLink}, {"TextSelectedBg", ImGuiCol_TextSelectedBg},
    {"DragDropTarget", ImGuiCol_DragDropTarget}, {"NavCursor", ImGuiCol_NavCursor},
    {"NavWindowingDimBg", ImGuiCol_NavWindowingDimBg}, {"ModalWindowDimBg", ImGuiCol_ModalWindowDimBg},
};

bool ApplyFloatStyle(const std::string& key, float v) {
    ImGuiStyle& s = ImGui::GetStyle();
    if (key == "WindowRounding") s.WindowRounding = v;
    else if (key == "ChildRounding") s.ChildRounding = v;
    else if (key == "PopupRounding") s.PopupRounding = v;
    else if (key == "FrameRounding") s.FrameRounding = v;
    else if (key == "ScrollbarRounding") s.ScrollbarRounding = v;
    else if (key == "GrabRounding") s.GrabRounding = v;
    else if (key == "TabRounding") s.TabRounding = v;
    else if (key == "WindowBorderSize") s.WindowBorderSize = v;
    else if (key == "ChildBorderSize") s.ChildBorderSize = v;
    else if (key == "PopupBorderSize") s.PopupBorderSize = v;
    else if (key == "FrameBorderSize") s.FrameBorderSize = v;
    else if (key == "TabBarBorderSize") s.TabBarBorderSize = v;
    else return false;
    return true;
}

} // namespace

void ApplyPbzTheme(float windowOpacity) {
    s_lastWindowOpacity = windowOpacity;
    ImGuiStyle& s = ImGui::GetStyle();

    // Sharp, hairline-outlined geometry - the game's HUD frames everything
    // with 1px strokes and has no rounded corner anywhere.
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.TabRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.GrabRounding = 0.0f;

    s.WindowBorderSize = 1.0f;   // silver hairline around the panel
    s.ChildBorderSize = 0.0f;    // inner regions divide by color, not stroke
    s.PopupBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;    // every widget outlined like a HUD bar
    s.TabBarBorderSize = 1.0f;
    s.TabBarOverlineSize = 2.0f; // selected-tab gold underline thickness

    // Kept identical to the pre-theme layout so the host's exported-rect
    // automation keeps hitting the same controls.
    s.WindowPadding = ImVec2(12, 10);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 6);
    s.ItemInnerSpacing = ImVec2(8, 6);
    s.ScrollbarSize = 11.0f;
    s.WindowTitleAlign = ImVec2(0.5f, 0.5f);  // centered title, like boss nameplates
    s.SeparatorTextBorderSize = 1.0f;
    s.SeparatorTextAlign = ImVec2(0.0f, 0.5f);

    s.FontScaleMain = 1.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_Text]                      = kRice;
    c[ImGuiCol_TextDisabled]              = kRiceDim;
    c[ImGuiCol_WindowBg]                  = WithAlpha(kPanel, windowOpacity);
    c[ImGuiCol_ChildBg]                   = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg]                   = WithAlpha(ImVec4(0.075f, 0.071f, 0.066f, 1.0f), 0.97f);
    c[ImGuiCol_Border]                    = WithAlpha(kSilverDim, 0.55f);
    c[ImGuiCol_BorderShadow]              = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]                   = kFrame;
    c[ImGuiCol_FrameBgHovered]            = kFrameHover;
    c[ImGuiCol_FrameBgActive]             = kFrameActive;
    c[ImGuiCol_TitleBg]                   = kInk;
    c[ImGuiCol_TitleBgActive]             = ImVec4(0.078f, 0.071f, 0.059f, 1.0f);  // warm shift when focused
    c[ImGuiCol_TitleBgCollapsed]          = WithAlpha(kInk, 0.65f);
    c[ImGuiCol_MenuBarBg]                 = kInk;
    c[ImGuiCol_ScrollbarBg]               = WithAlpha(kInk, 0.90f);
    c[ImGuiCol_ScrollbarGrab]             = ImVec4(0.227f, 0.212f, 0.180f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered]      = kGold;
    c[ImGuiCol_ScrollbarGrabActive]       = kGoldBright;
    c[ImGuiCol_CheckMark]                 = kCinnabar;            // checkbox = cinnabar seal mark
    c[ImGuiCol_SliderGrab]                = kSilver;              // HUD health-bar silver
    c[ImGuiCol_SliderGrabActive]          = kGoldBright;
    c[ImGuiCol_Button]                    = kButton;
    c[ImGuiCol_ButtonHovered]             = kButtonHov;
    c[ImGuiCol_ButtonActive]              = kButtonAct;
    c[ImGuiCol_Header]                    = kFrameHover;
    c[ImGuiCol_HeaderHovered]             = kButtonHov;
    c[ImGuiCol_HeaderActive]              = kButtonAct;
    c[ImGuiCol_Separator]                 = WithAlpha(kSilverDim, 0.45f);
    c[ImGuiCol_SeparatorHovered]          = kGold;
    c[ImGuiCol_SeparatorActive]           = kGoldBright;
    c[ImGuiCol_ResizeGrip]                = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ResizeGripHovered]         = WithAlpha(kGold, 0.60f);
    c[ImGuiCol_ResizeGripActive]          = WithAlpha(kGoldBright, 0.80f);
    c[ImGuiCol_InputTextCursor]           = kGoldBright;
    c[ImGuiCol_TabHovered]                = kFrameHover;
    c[ImGuiCol_Tab]                       = ImVec4(0.098f, 0.090f, 0.078f, 1.0f);
    c[ImGuiCol_TabSelected]               = ImVec4(0.133f, 0.122f, 0.098f, 1.0f);
    c[ImGuiCol_TabSelectedOverline]       = kGold;                // gold underline on the active tab
    c[ImGuiCol_TabDimmed]                 = ImVec4(0.078f, 0.071f, 0.063f, 1.0f);
    c[ImGuiCol_TabDimmedSelected]         = ImVec4(0.133f, 0.122f, 0.098f, 1.0f);
    c[ImGuiCol_TabDimmedSelectedOverline] = WithAlpha(kGold, 0.40f);
    c[ImGuiCol_PlotLines]                 = WithAlpha(kGold, 0.80f);
    c[ImGuiCol_PlotLinesHovered]          = kGoldBright;
    c[ImGuiCol_PlotHistogram]             = WithAlpha(kGold, 0.55f);
    c[ImGuiCol_PlotHistogramHovered]      = kGoldBright;
    c[ImGuiCol_TableHeaderBg]             = ImVec4(0.125f, 0.114f, 0.094f, 1.0f);
    c[ImGuiCol_TableBorderStrong]         = ImVec4(0.290f, 0.267f, 0.220f, 1.0f);
    c[ImGuiCol_TableBorderLight]          = ImVec4(0.200f, 0.184f, 0.153f, 1.0f);
    c[ImGuiCol_TableRowBg]                = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]             = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_TextLink]                  = kGoldBright;
    c[ImGuiCol_TextSelectedBg]            = WithAlpha(kGold, 0.35f);
    c[ImGuiCol_TreeLines]                 = ImVec4(0.290f, 0.267f, 0.220f, 1.0f);
    c[ImGuiCol_DragDropTarget]            = kGold;
    c[ImGuiCol_NavCursor]                 = WithAlpha(kGold, 0.90f);
    c[ImGuiCol_NavWindowingHighlight]     = WithAlpha(kRice, 0.70f);
    c[ImGuiCol_NavWindowingDimBg]         = WithAlpha(kInk, 0.65f);
    c[ImGuiCol_ModalWindowDimBg]          = WithAlpha(kInk, 0.70f);
}

void ApplyThemeByName(const std::string& nameUtf8) {
    // Always re-base on the built-in style first: switching between theme
    // files (or back to "ink") must not inherit the previous file's leftovers.
    ApplyPbzTheme(s_lastWindowOpacity);
    if (nameUtf8.empty() || nameUtf8 == "ink") return;

    // Stems only - the name ends up in a path below.
    for (unsigned char ch : nameUtf8) {
        if (!(isalnum(ch) || ch == '_' || ch == '-' || ch == ' ')) {
            Log("[theme] rejected theme name: %s", nameUtf8.c_str());
            return;
        }
    }
    const std::wstring path = ModRoot() + L"\\themes\\" + Utf8ToWide(nameUtf8) + L".json";
    std::string text = ReadFileText(path);
    std::map<std::string, std::string> raw;
    if (text.empty() || !ParseFlatJsonRaw(text, raw)) {
        Log("[theme] theme file missing or unparsable: %ls", path.c_str());
        return;
    }

    int applied = 0;
    ImGuiStyle& s = ImGui::GetStyle();
    for (auto& kv : raw) {
        bool matched = false;
        for (const ColName& cn : kColNames) {
            if (kv.first == cn.name) {
                bool ok = false;
                ImVec4 c = FromHex(kv.second, &ok);
                if (ok) {
                    s.Colors[cn.col] = c;
                    ++applied;
                }
                matched = true;
                break;
            }
        }
        if (matched) continue;
        if (ApplyFloatStyle(kv.first, (float)strtod(kv.second.c_str(), nullptr))) ++applied;
    }
    Log("[theme] applied %d overrides from %ls", applied, path.c_str());
}

namespace {
std::vector<std::string> s_themeNames;
bool s_themeNamesLoaded = false;
} // namespace

void RefreshThemeNames() {
    // M4: FindFirstFileW on every menu frame was wasted disk I/O; the scan now
    // runs only when the picker opens (or a theme changes) yet still sees files
    // added to themes\ in the meantime.
    s_themeNames.clear();
    s_themeNames.push_back("ink");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((ModRoot() + L"\\themes\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring stem = fd.cFileName;
            size_t dot = stem.rfind(L'.');
            if (dot != std::wstring::npos) stem.resize(dot);
            int need = WideCharToMultiByte(CP_UTF8, 0, stem.c_str(), (int)stem.size(), nullptr, 0, nullptr, nullptr);
            std::string u(need, 0);
            WideCharToMultiByte(CP_UTF8, 0, stem.c_str(), (int)stem.size(), u.data(), need, nullptr, nullptr);
            if (u != "ink") s_themeNames.push_back(u);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(s_themeNames.begin() + 1, s_themeNames.end());
    s_themeNamesLoaded = true;
}

const std::vector<std::string>& ListThemeNames() {
    if (!s_themeNamesLoaded) RefreshThemeNames();
    return s_themeNames;
}

void PushCapturingStyle() {
    ImGui::PushStyleColor(ImGuiCol_Button, WithAlpha(kCinnabarDp, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, WithAlpha(kCinnabar, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kCinnabar);
    ImGui::PushStyleColor(ImGuiCol_Border, WithAlpha(kCinnabar, 0.80f));
}

void PopCapturingStyle() {
    ImGui::PopStyleColor(4);
}

} } // namespace pbzfm::theme
