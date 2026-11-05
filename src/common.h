// PBZ-FM common declarations shared by all translation units.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <string>

namespace pbzfm {

// ---- shared state (defined in pbzfm.cpp) ----
extern std::atomic<bool> g_running;      // module lifecycle
extern std::atomic<bool> g_menuOpen;     // overlay visible (hook threads read)
extern std::atomic<bool> g_wantToggle;   // hook -> render thread: toggle menu
extern std::atomic<bool> g_closeRequest; // menu ui -> render thread: close menu
extern std::atomic<UINT> g_toggleVk;     // current toggle virtual-key (hook reads)
extern std::atomic<int>  g_captureMode;  // 0 = none, 1 = waiting for a new key
extern std::atomic<UINT> g_capturedVk;   // capture result (0 = none)
extern std::atomic<unsigned> g_capturedMods; // PBZFM_MOD_* bits held at capture completion
extern std::atomic<int>  g_captureIndex;  // CAP_PASSTHROUGH target slot (-1 = append)
extern std::atomic<int>  g_captureKind;  // CaptureKind in schema.h (0 none, 1 toggle, 2 schema key)
extern std::atomic<int>  g_btnCliX;      // demo button rect, overlay client coords
extern std::atomic<int>  g_btnCliY;      // (menu.cpp writes, overlay converts to screen)
extern std::atomic<int>  g_btnScrX;      // demo button center, screen coords (-1 = unknown)
extern std::atomic<int>  g_btnScrY;
extern std::atomic<int>  g_hookMoves;    // real mouse moves seen by the hook while menu open (diagnostic)
extern std::atomic<void*> g_gameHwnd;    // game window handle; the hook's passthrough borrow window
                                         // ends when the foreground returns to this window
extern std::atomic<int>  g_inputReset;   // hook -> render thread: ImGui input state went stale (passthrough)

extern const char* const kVersion;

// ---- logging (thread-safe, appends to <Documents>\PBZ\Logs\PBZFM.log, falls back to <mod root>\PBZFM.log) ----
void Log(const char* fmt, ...);

// ---- paths ----
std::wstring DllDir();                        // directory containing this DLL
std::wstring ModRoot();                       // parent of \dlls\ when loaded as a UE4SS mod, else DllDir()
std::wstring RootFile(const wchar_t* name);   // ModRoot() + name (fallback: beside the DLL)

// M1: the single write primitive for config/schema stores. Writes a temp file,
// flushes it, then replaces the destination atomically; logs ERROR on failure
// so "saved" is never printed for a write that did not reach the disk.
bool WriteFileAtomic(const std::wstring& path, const std::string& utf8);

} // namespace pbzfm
