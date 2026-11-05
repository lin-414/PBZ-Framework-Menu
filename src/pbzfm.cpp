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
#include <string>
#include <vector>
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
std::atomic<int>  g_captureIndex{ -1 };
std::atomic<int>  g_btnCliX{ -1 };
std::atomic<int>  g_btnCliY{ -1 };
std::atomic<int>  g_btnScrX{ -1 };
std::atomic<int>  g_btnScrY{ -1 };
std::atomic<int>  g_hookMoves{ 0 };
std::atomic<void*> g_gameHwnd{ nullptr };
std::atomic<int>  g_inputReset{ 0 };

#ifndef PBZFM_VERSION_STR
#define PBZFM_VERSION_STR "0.5.2"
#endif
const char* const kVersion = PBZFM_VERSION_STR;

namespace {

HMODULE g_self = nullptr;
HANDLE g_initThread = nullptr;
// H9: set when InputStop/OverlayStop could not confirm a clean shutdown (a
// thread may still execute our code). RequestShutdown then pins the module.
std::atomic<bool> g_unloadUnsafe{ false };

// H9: a failed shutdown leaves a thread that can still call into this DLL.
// FreeLibrary then unmaps code under a running thread (instant crash). Pinning
// makes every later FreeLibrary a no-op: the module leaks until process exit,
// which is strictly better than a use-after-free of our .text.
void PinModuleForUnsafeUnload(const char* why) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            (LPCWSTR)(uintptr_t)&PinModuleForUnsafeUnload, &pinned)) {
        Log("ERROR: %s; module pin failed err=%lu - host MUST NOT FreeLibrary this DLL", why,
            GetLastError());
        return;
    }
    Log("ERROR: %s; module pinned - FreeLibrary is now a no-op to avoid unmapped-code crash", why);
}

std::mutex g_logMutex;
FILE* g_logFile = nullptr;
std::wstring g_logPath;
bool g_logPathResolved = false;

// L7: bounded log + de-duplication state. Rotation keeps at most 1 MiB live and
// one PBZFM.log.1; an identical message inside the window is counted instead of
// written, so a stuck loop cannot flood the disk. All guarded by g_logMutex.
constexpr long long kLogMaxBytes = 1LL << 20;   // 1 MiB live file
constexpr DWORD kLogDedupeMs = 5000;
constexpr unsigned kDebugOutPerSec = 20;        // debugger-output budget
std::string g_logLastMsg;
DWORD g_logLastTick = 0;
unsigned g_logRepeatSuppressed = 0;
DWORD g_dbgWindowTick = 0;
unsigned g_dbgWindowCount = 0;
bool g_logRotateWarned = false;

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
        // B2: a failed init must not leave the game window subclassed for the
        // rest of the session. OverlayStop() is safe here (its thread handle
        // is already closed/empty) and is the single place that detaches.
        if (!OverlayStop()) {
            g_unloadUnsafe.store(true, std::memory_order_relaxed);
            Log("ERROR: subclass rollback after init failure failed");
        }
        return 0;
    }
    InputStart();
    Log("prototype ready: press %s to toggle the menu", Config::VkName(g_cfg.toggleVk));

    while (g_running.load(std::memory_order_relaxed)) Sleep(200);

    // H9: InputStop unhooks before joining and reports a timeout; OverlayStop
    // keeps its B2 contract (false = render thread/restore unresolved). Either
    // failure means a thread may still be inside this DLL, so the module is
    // marked unsafe to unload instead of pretending the shutdown was clean.
    const bool inputOk = InputStop();
    const bool overlayOk = OverlayStop();
    if (inputOk && overlayOk) {
        Log("PBZFM unloaded cleanly");
    } else {
        g_unloadUnsafe.store(true, std::memory_order_relaxed);
        Log("ERROR: shutdown incomplete (input=%d overlay=%d) - module must stay loaded",
            (int)inputOk, (int)overlayOk);
    }
    return 0;
}

} // namespace

// L7: called with g_logMutex held and g_logFile open. Rotating at 1 MiB keeps a
// single PBZFM.log.1; when the move fails, say so once and keep appending to the
// same file instead of silently dropping every later line.
static void RotateLogLocked() {
    fclose(g_logFile);
    g_logFile = nullptr;
    const std::wstring backup = g_logPath + L".1";
    if (MoveFileExW(g_logPath.c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING) == 0) {
        g_logFile = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYNO);
        if (g_logFile && !g_logRotateWarned) {
            g_logRotateWarned = true;
            const char* msg = "[log] ERROR: rotation failed - continuing on the same file\n";
            fwrite(msg, 1, strlen(msg), g_logFile);
            fflush(g_logFile);
        }
        return;
    }
    g_logFile = _wfsopen(g_logPath.c_str(), L"ab", _SH_DENYNO);
}

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
    // L7: rotate at the size limit before writing this line; a failed reopen
    // must not fall through into a null FILE*.
    if (_ftelli64(g_logFile) >= kLogMaxBytes) RotateLogLocked();
    if (!g_logFile) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[80];
    snprintf(stamp, sizeof(stamp), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    char msg[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, args);
    va_end(args);

    const DWORD nowTick = GetTickCount();
    // L7: an identical message inside the window is counted, not written; the
    // summary is emitted right before the next distinct line so suppression is
    // visible without flooding the file.
    if (g_logLastTick && msg == g_logLastMsg && nowTick - g_logLastTick < kLogDedupeMs) {
        ++g_logRepeatSuppressed;
        return;
    }
    if (g_logRepeatSuppressed) {
        char sum[128];
        snprintf(sum, sizeof(sum), "%s[log] previous message repeated %u time(s)\n",
                 stamp, g_logRepeatSuppressed);
        fwrite(sum, 1, strlen(sum), g_logFile);
        g_logRepeatSuppressed = 0;
    }
    fwrite(stamp, 1, strlen(stamp), g_logFile);
    fwrite(msg, 1, strlen(msg), g_logFile);
    fputc('\n', g_logFile);
    fflush(g_logFile);
    g_logLastMsg.assign(msg);
    g_logLastTick = nowTick;

    // L7: OutputDebugString only when a debugger is attached and within a
    // per-second budget, so input/render hot paths do not pay for it.
    if (IsDebuggerPresent()) {
        if (nowTick - g_dbgWindowTick >= 1000) {
            g_dbgWindowTick = nowTick;
            g_dbgWindowCount = 0;
        }
        if (++g_dbgWindowCount <= kDebugOutPerSec) {
            char line[1104];
            snprintf(line, sizeof(line), "%s%s", stamp, msg);
            OutputDebugStringA(line);
        }
    }
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
            // B2: the init thread is the only one that joins the render thread
            // and restores the game WndProc (OverlayStop). A timeout means the
            // restore has not happened yet: report it explicitly instead of
            // pretending the module is unloaded, and keep the handle so a
            // running thread never has its code freed underneath it.
            Log("ERROR: init thread still running after 5s - overlay/WndProc shutdown unresolved");
            // H9: the init thread may still be inside InputStop/OverlayStop.
            PinModuleForUnsafeUnload("init thread still running after 5s");
            return;
        }
        CloseHandle(g_initThread);
        g_initThread = nullptr;
    }
    if (g_unloadUnsafe.load(std::memory_order_relaxed))
        PinModuleForUnsafeUnload("shutdown reported incomplete");
}

} // namespace pbzfm

using namespace pbzfm;

static void AppendLogTailToDebugger() {}

#if PBZFM_TEST_HOOKS
// M8: the test-only exports take UTF-8 paths (the host speaks UTF-8); keep the
// conversion local to the test block so the shipped code carries no dead helper.
static std::wstring WideFromUtf8(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    if (!w.empty() && w.back() == 0) w.pop_back();
    return w;
}
#endif

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        g_self = self;
        g_running.store(true, std::memory_order_relaxed);
        g_initThread = CreateThread(nullptr, 0, InitThreadProc, nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        // Loader lock: only signal. The WndProc restore runs exactly once in
        // OverlayStop() on the init thread; requesting it here as well would
        // mean SendMessage under the loader lock and a duplicate restore.
        // Clean unload goes through PBZFM_Shutdown()/uninstall_mod().
        g_running.store(false, std::memory_order_relaxed);
    }
    return TRUE;
}

extern "C" {

__declspec(dllexport) void PBZFM_Shutdown() { pbzfm::RequestShutdown(); }

// M8: automation-only exports. PBZFM_TEST_HOOKS is OFF in release builds, so
// these injection hooks never reach players; the host regression turns it on.
#if PBZFM_TEST_HOOKS

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

// M1 regression: drive the shared atomic-write primitive from the host so the
// success path and the failure path (old store must stay intact) are both
// assertable without launching the game.
__declspec(dllexport) bool PBZFM_TestAtomicWrite(const char* path_utf8, const char* utf8) {
    if (!path_utf8 || !utf8) return false;
    return pbzfm::WriteFileAtomic(WideFromUtf8(path_utf8), std::string(utf8));
}

// M10 regression: encode a fully transparent BGRA buffer (DIB pixels carry
// alpha 0) and write the PNG, so the host can read the pixels back and assert
// the encoder forced alpha to 255.
__declspec(dllexport) bool PBZFM_TestWriteAlphaPng(const char* path_utf8, int w, int h) {
    if (!path_utf8 || w <= 0 || h <= 0 || w > 4096 || h > 4096) return false;
    std::vector<unsigned char> bgra((size_t)w * (size_t)h * 4, 0);   // alpha deliberately 0
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i + 0] = 0x20;
        bgra[i + 1] = 0x80;
        bgra[i + 2] = 0xF0;
    }
    std::vector<unsigned char> png;
    if (!pbzfm::EncodeOpaqueBgraPng(bgra.data(), w, h, png)) return false;
    return pbzfm::WriteFileAtomic(WideFromUtf8(path_utf8), std::string(png.begin(), png.end()));
}

#endif   // PBZFM_TEST_HOOKS

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
