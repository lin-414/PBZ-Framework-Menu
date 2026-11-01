#pragma once
#include <string>
#include <windows.h>

namespace pbzfm {

struct Config {
    // framework
    std::string language = "zh-CN";   // UTF-8, matches i18n/ImGui
    UINT toggleVk = VK_F8;
    // ui
    float uiScale   = 1.0f;   // style.FontScaleMain
    float uiOpacity = 0.94f;  // menu window background alpha
    // demo settings (stand in for real mod settings until the schema API exists)
    float fov     = 90.0f;
    bool  freeze  = false;
    int   quality = 1;        // 0..3
    long  clicks  = 0;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;

    static const char* VkName(UINT vk);                    // "F8", "A", "NUMPAD4", "0x9A"...
    static UINT VkFromName(const std::wstring& name);      // 0 when unknown
};

} // namespace pbzfm
