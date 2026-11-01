// Demo client mod: proves the PBZFM C API works from a separate DLL with zero
// dependencies on the framework's build (no ImGui, no UE4SS). Registers a
// section with a schema page + a custom widget-API page + a gameplay hotkey,
// and writes every observable state change to demo_mod_state.txt so the host
// test can assert on files instead of pixels.
#include "../sdk/pbzfm.h"

#include <windows.h>
#include <cstdio>

namespace {

PBZFM_Api api;
bool apiReady = false;
PBZFM_Config cfg = nullptr;

int  clicks = 0;
int  hotkeys = 0;
int  boolVal = 0;
float btnRect[4] = { -1, -1, -1, -1 };

void SaveState() {
    FILE* f = nullptr;
    if (fopen_s(&f, "demo_mod_state.txt", "w") != 0 || !f) return;
    fprintf(f, "clicks=%d\nhotkeys=%d\nbool=%d\n", clicks, hotkeys, boolVal);
    fclose(f);
}

void OnChange(const char* key, void*) {
    // Schema values live in the framework; read back whatever changed.
    if (key && strcmp(key, "enable_fx") == 0) {
        boolVal = api.GetBool(cfg, "enable_fx");
        FILE* f = nullptr;
        if (fopen_s(&f, "demo_mod_change.log", "a") == 0 && f) {
            fprintf(f, "change enable_fx -> boolVal=%d\n", boolVal);
            fclose(f);
        }
    }
    SaveState();
}

void RenderCustomPage(void*) {
    api.Text("Widget API demo page (drawn by demo_mod.dll through the PBZFM C API)");
    api.Separator();
    if (api.Button("demo_button")) {
        clicks++;
        SaveState();
    }
    api.LastItemRect(btnRect);
    api.SameLine();
    char line[64];
    sprintf_s(line, sizeof(line), "clicks: %d", clicks);
    api.Text(line);
    api.TextDisabled("Press F9 in gameplay to fire the registered hotkey.");
}

void OnHotkey(void*) {
    hotkeys++;
    SaveState();
}

DWORD WINAPI InitThread(LPVOID) {
    for (int i = 0; i < 200; ++i) {
        if (PBZFM_LoadApi(&api)) { apiReady = true; break; }
        Sleep(100);
    }
    if (!apiReady) return 0;

    // Schema-backed section (auto-rendered settings form).
    api.SetSection("演示模组");
    cfg = api.ConfigCreate("demo", "设置");
    api.ConfigAddBool(cfg, "enable_fx", "启用特效", 0);
    api.ConfigAddFloat(cfg, "fov", "视野 (FOV)", 90.0f, 15.0f, 150.0f, 1.0f, "%.0f");
    const char* qualities[4] = { "低", "中", "高", "自定义" };
    api.ConfigAddEnum(cfg, "quality", "画质预设", 1, qualities, 4);
    api.ConfigAddKey(cfg, "shot_key", "截图键", '9');
    api.ConfigOnChange(cfg, OnChange, nullptr);

    // v3 path API (optional export): a nested category under the same mod.
    // The sidebar renders 演示模组 as an expandable node holding 设置 + 高级参数.
    if (api.ConfigCreatePath) {
        if (PBZFM_Config adv = api.ConfigCreatePath("demo.adv", "演示模组/高级参数")) {
            api.ConfigAddBool(adv, "grain", "胶片颗粒", 0);
            api.ConfigAddInt(adv, "vignette", "暗角强度", 2, 0, 5);
        }
    }

    // Custom widget-API section as its own sidebar entry so the host can
    // navigate deterministically through exported rects.
    api.SetSection("演示模组 · 自定义");
    api.AddSectionItem("自定义页", RenderCustomPage, nullptr);

    api.AddHotkey(VK_F9, OnHotkey, nullptr);

    boolVal = api.GetBool(cfg, "enable_fx");
    SaveState();

    // Prove the value path programmatically: set -> change callback -> dirty
    // store, persisted when the menu next closes (or by ConfigSave).
    Sleep(300);
    api.SetBool(cfg, "enable_fx", 1);
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(GetModuleHandleW(nullptr));
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) bool PBZFMDEMO_GetButton(long* x, long* y) {
    if (!x || !y || btnRect[0] < 0) return false;
    *x = (long)((btnRect[0] + btnRect[2]) * 0.5f);
    *y = (long)((btnRect[1] + btnRect[3]) * 0.5f);
    return true;
}

extern "C" __declspec(dllexport) bool PBZFMDEMO_Ready() {
    return apiReady;
}
