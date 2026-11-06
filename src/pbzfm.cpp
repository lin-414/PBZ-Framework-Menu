// PBZFM module entry. Loaded as a UE4SS DLL mod (ue4ss\Mods\PBZFM\dlls\PBZFM.dll)
// or by any host. DllMain only spawns the init thread: no library loads, no
// window work under the loader lock (PBZPhotoMode lesson 1.1.0).
#include "common.h"
#include "config.h"
#include "i18n.h"
#include "game_window.h"
#include "input.h"
#include "overlay.h"
#include "registry.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <cwctype>
#include <shlobj.h>

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
std::atomic<int>  g_btnCliX{ -1 };
std::atomic<int>  g_btnCliY{ -1 };
std::atomic<int>  g_btnScrX{ -1 };
std::atomic<int>  g_btnScrY{ -1 };
std::atomic<int>  g_hookMoves{ 0 };
std::atomic<void*> g_gameHwnd{ nullptr };
std::atomic<int>  g_inputReset{ 0 };

#ifndef PBZFM_VERSION_STR
#define PBZFM_VERSION_STR "0.6.0"
#endif
const char* const kVersion = PBZFM_VERSION_STR;

namespace {

HMODULE g_self = nullptr;
HANDLE g_initThread = nullptr;

std::mutex g_logMutex;
FILE* g_logFile = nullptr;
std::wstring g_logPath;
bool g_logPathResolved = false;

// Preferred log location: <Documents>\PBZ\Logs, created on demand. Empty when unavailable.
std::wstring DocumentsLogsDir() {
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return {};
    std::wstring dir = docs;
    CoTaskMemFree(docs);
    dir += L"\\PBZ";
    CreateDirectoryW(dir.c_str(), nullptr);
    dir += L"\\Logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return {};
    return dir;
}

std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
}

// Statics referenced by the render thread for the whole process lifetime.
Config g_cfg;
I18n g_i18n;
GameWindow g_gw;

DWORD WINAPI InitThreadProc(LPVOID) {
    Log("PBZFM %s loading (pid=%lu tid=%lu)", kVersion, GetCurrentProcessId(), GetCurrentThreadId());

    // UE4SS can load us before the game creates its window; poll for up to 180 s.
    bool found = false;
    for (int i = 0; i < 360 && g_running.load(std::memory_order_relaxed); ++i) {
        if (g_gw.Find()) { found = true; break; }
        Sleep(500);
    }
    if (!found) {
        Log("no UnrealWindow in this process after 180s - staying dormant");
        return 0;
    }

    const std::wstring iniPath = RootFile(L"PBZFM.ini");
    if (g_cfg.Load(iniPath)) {
        Log("config loaded: %ls", iniPath.c_str());
    } else {
        Log("config not found, defaults in use (%ls is written on save)", iniPath.c_str());
    }
    g_toggleVk.store(g_cfg.toggleVk, std::memory_order_relaxed);
    // Published before InputStart: the hook thread reads the chord list from
    // then on (the SRWLock in input.cpp keeps a future live reconfigure safe).
    InputSetPassthrough(g_cfg.passthroughKeys, g_cfg.passthroughReplay);
    g_i18n.Load(ModRoot() + L"\\languages", Utf8ToWide(g_cfg.language));

    if (!OverlayStart(&g_gw, &g_cfg, &g_i18n, iniPath)) {
        Log("overlay failed to start - module dormant (see earlier log lines)");
        return 0;
    }
    InputStart();
    Log("prototype ready: press %s to toggle the menu", Config::VkName(g_cfg.toggleVk));

    while (g_running.load(std::memory_order_relaxed)) Sleep(200);

    InputStop();
    OverlayStop();
    Log("PBZFM unloaded cleanly");
    return 0;
}

} // namespace

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logPathResolved) {
        std::wstring docs = DocumentsLogsDir();
        g_logPath = docs.empty() ? RootFile(L"PBZFM.log") : docs + L"\\PBZFM.log";
        g_logPathResolved = true;
    }
    if (!g_logFile) {
        // _SH_DENYNO so log tailing works while the game is running (fopen/fopen_s deny sharing).
        g_logFile = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYNO);
        if (!g_logFile) {
            const std::wstring fallback = RootFile(L"PBZFM.log");
            if (fallback == g_logPath) return;
            g_logPath = fallback; // Documents location unusable - log beside the mod instead
            g_logFile = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYNO);
            if (!g_logFile) return;
        }
    }
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[80];
    snprintf(stamp, sizeof(stamp), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fwrite(stamp, 1, strlen(stamp), g_logFile);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_logFile, fmt, args);
    va_end(args);
    fputc('\n', g_logFile);
    fflush(g_logFile);

    char line[1024];
    int n = snprintf(line, sizeof(line), "%s", stamp);
    va_start(args, fmt);
    _vsnprintf_s(line + n, sizeof(line) - (size_t)n, _TRUNCATE, fmt, args);
    va_end(args);
    OutputDebugStringA(line);
}

std::wstring DllDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(g_self, path, MAX_PATH);
    std::wstring s = path;
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"." : s.substr(0, p);
}

std::wstring ModRoot() {
    std::wstring d = DllDir();
    if (d.size() >= 5) {
        std::wstring tail = d.substr(d.size() - 5);
        std::transform(tail.begin(), tail.end(), tail.begin(), ::towlower);
        if (tail == L"\\dlls") return d.substr(0, d.size() - 5);
    }
    return d;
}

std::wstring RootFile(const wchar_t* name) {
    std::wstring root = ModRoot();
    DWORD attr = GetFileAttributesW(root.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) root = DllDir();
    return root + L"\\" + name;
}

void RequestShutdown() {
    g_running.store(false, std::memory_order_relaxed);
    if (g_initThread) {
        if (WaitForSingleObject(g_initThread, 5000) != WAIT_OBJECT_0) {
            Log("WARNING: init thread did not exit within 5s");
        }
        CloseHandle(g_initThread);
        g_initThread = nullptr;
    }
}

} // namespace pbzfm

using namespace pbzfm;

static void AppendLogTailToDebugger() {}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        g_self = self;
        g_running.store(true, std::memory_order_relaxed);
        g_initThread = CreateThread(nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Loader lock: only signal. Clean shutdown goes through PBZFM_Shutdown().
        g_running.store(false, std::memory_order_relaxed);
    }
    return TRUE;
}

extern "C" {

__declspec(dllexport) void PBZFM_Shutdown() { pbzfm::RequestShutdown(); }

// Test hook: current screen position of the demo button (host clicks here).
__declspec(dllexport) bool PBZFM_GetDemoButton(long* x, long* y) {
    if (!x || !y) return false;
    *x = g_btnScrX.load(std::memory_order_relaxed);
    *y = g_btnScrY.load(std::memory_order_relaxed);
    return *x >= 0 && *y >= 0;
}

// Test hooks: synthetic input for the host test (input.h InputPushEvent).
// They feed the same event queue the low-level hooks write, so the render
// thread and everything downstream are exercised for real.
__declspec(dllexport) void PBZFM_TestInjectMove(int sx, int sy) {
    pbzfm::InputEvent e{};
    e.type = pbzfm::InputEvent::Move;
    e.wx = (float)sx;
    e.wy = (float)sy;
    pbzfm::InputPushEvent(e);
}

__declspec(dllexport) void PBZFM_TestInjectButton(int btn, bool down) {
    pbzfm::InputEvent e{};
    e.type = pbzfm::InputEvent::Btn;
    e.key = btn;
    e.down = down;
    pbzfm::InputPushEvent(e);
}

__declspec(dllexport) void PBZFM_TestToggleMenu(bool open) {
    if (open) {
        g_wantToggle.store(true, std::memory_order_release);
    } else {
        g_closeRequest.store(true, std::memory_order_release);
    }
}

// Mirrors what the keyboard hook does for a closed-menu key (registry
// hotkeys fire on down edges).
__declspec(dllexport) bool PBZFM_TestDispatchHotkey(unsigned vk) {
    return pbzfm::RegistryDispatchHotkey(vk, false, 0);
}

// Test probe: is the screenshot-tool passthrough borrow window open right now?
__declspec(dllexport) int PBZFM_TestPassthroughActive() {
    return pbzfm::InputPassthroughActive() ? 1 : 0;
}

// Test hook: replace the passthrough chord list with a single chord so the
// host scenario never pops a live PixPin/NVIDIA capture overlay.
__declspec(dllexport) void PBZFM_TestSetPassthrough(unsigned vk, bool ctrl, bool shift, bool alt, bool win, bool replay) {
    std::vector<pbzfm::HotkeyChord> chords{ pbzfm::HotkeyChord{ vk, ctrl, shift, alt, win } };
    pbzfm::InputSetPassthrough(chords, replay);
}

// UE4SS C++ mod lifecycle contract (CppMod.cpp requires both exports; without
// them UE4SS FreeLibrary's the DLL right after LoadLibraryExW). PBZFM manages
// its own lifecycle from the DllMain thread, so start_mod returns nullptr and
// UE4SS skips every lifecycle callback (all are null-guarded in CppMod.cpp).
__declspec(dllexport) void* start_mod() {
    Log("UE4SS lifecycle: start_mod() -> nullptr (self-managed mod)");
    return nullptr;
}

__declspec(dllexport) void uninstall_mod(void* mod) {
    (void)mod;
    Log("UE4SS lifecycle: uninstall_mod()");
    RequestShutdown();
}

} // extern "C"
