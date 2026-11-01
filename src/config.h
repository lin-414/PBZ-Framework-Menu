#pragma once
#include <string>
#include <vector>
#include <windows.h>

namespace pbzfm {

// A hotkey chord as written in PBZFM.ini ("Alt+P", "Ctrl+Shift+F5"). Modifiers
// must match exactly (a held extra Shift does not trigger an Alt+P chord).
struct HotkeyChord {
    UINT vk = 0;
    bool ctrl = false, shift = false, alt = false, win = false;
};

struct Config {
    // framework
    std::string language = "zh-CN";   // UTF-8, matches i18n/ImGui
    UINT toggleVk = VK_F8;
    // Chords handed to global screenshot tools (PixPin / NVIDIA App) instead
    // of being swallowed while the menu owns the input. Empty = feature off.
    std::vector<HotkeyChord> passthroughKeys = {
        { 'P',   true,  false, false, false },  // Ctrl+P (PixPin - match your own config)
        { VK_F1, false, false, true,  false },  // Alt+F1 (NVIDIA App screenshot)
    };
    // Swallowed modifier-downs are invisible to the rest of the system (async
    // key state and hotkey tracking alike), so passing only the final key can
    // never deliver the chord. Replay the full chord synthetically instead -
    // the injected events flow back through our own borrow window untouched.
    bool passthroughReplay = true;
    // ui
    float uiScale   = 1.0f;   // style.FontScaleMain
    float uiOpacity = 0.94f;  // menu window background alpha
    // Theme selector: "ink" = built-in 玄墨鎏金, else themes\<name>.json stem.
    std::string theme = "ink";
    // demo settings (stand in for real mod settings until the schema API exists)
    float fov     = 90.0f;
    bool  freeze  = false;
    int   quality = 1;        // 0..3
    long  clicks  = 0;

    bool Load(const std::wstring& path);
    bool Save(const std::wstring& path) const;

    static const char* VkName(UINT vk);                    // "F8", "A", "NUMPAD4", "0x9A"...
    static UINT VkFromName(const std::wstring& name);      // 0 when unknown
    static std::vector<HotkeyChord> ChordsFromText(const std::wstring& csv);  // tolerant; drops invalid items
    static std::string ChordToString(const HotkeyChord& c);                   // canonical "Alt+P" form
    // v2: "Ctrl+Alt+P" from a packed vk + PBZFM_MOD_* bitmask (schema key combos)
    static std::string KeyModsToString(unsigned vk, unsigned mods);
};

} // namespace pbzfm
