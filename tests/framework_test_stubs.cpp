// Test-only stubs for the framework_test link. The production TUs under test
// (registry/schema/config/input/api/files) reference the DLL entry's shared
// state, paths and subsystems; the unit test provides quiet stand-ins so it
// stays device-free and window-free. Nothing here changes production behavior -
// the DLL keeps its real definitions.
#include "common.h"
#include "i18n.h"
#include "textures.h"
#include "theme.h"

#include <string>

namespace pbzfm {

std::atomic<bool> g_running{ false };
std::atomic<bool> g_menuOpen{ false };
std::atomic<bool> g_wantToggle{ false };
std::atomic<bool> g_closeRequest{ false };
std::atomic<UINT> g_toggleVk{ VK_F8 };
std::atomic<int>  g_captureMode{ 0 };
std::atomic<int>  g_captureKind{ 0 };
std::atomic<UINT> g_capturedVk{ 0 };
std::atomic<unsigned> g_capturedMods{ 0 };
std::atomic<int>  g_captureIndex{ -1 };
std::atomic<int>  g_btnCliX{ -1 };
std::atomic<int>  g_btnCliY{ -1 };
std::atomic<int>  g_btnScrX{ -1 };
std::atomic<int>  g_btnScrY{ -1 };
std::atomic<int>  g_hookMoves{ 0 };
std::atomic<void*> g_gameHwnd{ nullptr };
std::atomic<int>  g_inputReset{ 0 };

const char* const kVersion = "0.5.2-test";

// Silent: the tests assert state, never log text.
void Log(const char*, ...) {}

namespace {

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(GetModuleHandleW(nullptr), path, MAX_PATH);
    std::wstring d(path);
    const size_t p = d.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring(L".") : d.substr(0, p);
}

std::wstring TempRoot() {
    // framework_test creates this directory before running; stores land here
    // instead of the game/mod data dir.
    static const std::wstring root = ExeDir() + L"\\framework_test_tmp";
    return root;
}

} // namespace

std::wstring DllDir() { return TempRoot(); }
std::wstring ModRoot() { return TempRoot(); }
std::wstring RootFile(const wchar_t* name) { return TempRoot() + L"\\" + name; }

namespace theme {
void PushCapturingStyle() {}
void PopCapturingStyle() {}
} // namespace theme

const char* I18n::T(const char* key) const { return key; }

// The texture runtime needs a D3D device; the H4 slot logic is covered through
// texture_slots.h instead (see framework_test.cpp).
void* TexturesLoad(const char* path) { (void)path; return nullptr; }
void  TexturesDispose(void*) {}
bool  TexturesDraw(void*, float, float) { return false; }
void  TexturesProcessPending() {}
void  TexturesShutdown() {}

} // namespace pbzfm
