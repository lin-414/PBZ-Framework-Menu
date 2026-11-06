// PBZFM test host: simulates the game for local verification without
// launching Phantom Blade Zero.
//  - registers a visible window of class "UnrealWindow" and LoadLibrary's
//    PBZFM.dll exactly like UE4SS would, plus demo_mod.dll (a zero-dependency
//    client that registers through the PBZFM C API)
//  - every click target is resolved through exported rects (section entries,
//    schema items, client button) - no pixel guessing
//  - asserts on demo_mod_state.txt + data/demo.json afterwards
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
// M10 regression: read the DLL-produced PNG back to assert its alpha channel.
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <windows.h>
#include <shlobj.h>
#include <dwmapi.h>

#include "layout.h"
#include <share.h>
#include <cerrno>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <utility>
#include <string>

namespace {
constexpr wchar_t kHostClass[] = L"UnrealWindow";
constexpr int kW = 1280, kH = 720;

HWND g_hwnd = nullptr;
HMODULE g_dll = nullptr;
HMODULE g_demo = nullptr;

typedef const char* (*PBZFM_VersionFn)();
typedef void (*PBZFM_ShutdownFn)();
typedef bool (*PBZFMDEMO_GetButtonFn)(long*, long*);

typedef int  (*PBZFM_pfn_int)();
typedef int  (*PBZFM_pfn_GetSectionName)(int, char*, int);
typedef int  (*PBZFM_pfn_GetRect1)(int, float*);
typedef int  (*PBZFM_pfn_GetRect0)(float*);
typedef void (*PBZFM_TestInjectMoveFn)(int, int);
typedef void (*PBZFM_TestInjectButtonFn)(int, bool);
typedef void (*PBZFM_TestToggleMenuFn)(bool);
typedef bool (*PBZFM_TestDispatchHotkeyFn)(unsigned);
typedef void (*PBZFM_TestSetPassthroughFn)(unsigned, bool, bool, bool, bool, bool);
typedef int  (*PBZFM_TestPtActiveFn)();

PBZFM_pfn_int pfnSectionCount;
PBZFM_pfn_GetSectionName pfnSectionName;
PBZFM_pfn_GetRect1 pfnSectionRect;
PBZFM_pfn_GetRect0 pfnLastSchemaRect;
PBZFM_TestInjectMoveFn pfnInjectMove;
PBZFM_TestInjectButtonFn pfnInjectButton;
PBZFM_TestToggleMenuFn pfnToggleMenu;
PBZFM_TestDispatchHotkeyFn pfnDispatchHotkey;
PBZFM_TestSetPassthroughFn pfnSetPassthrough;
PBZFM_TestPtActiveFn pfnPtActive;

// --synthetic: drive the menu through the DLL's test-injection exports
// instead of the real input stream, whose delivery depends on an
// uncontended input desktop (a human using the machine races the strokes).
bool g_synthetic = false;
int g_fail = 0;   // passthrough scenario result (exit code)

// B2 regression: shutdown runs on its own thread (like UE4SS's uninstall_mod)
// so the window thread stays free to service the WndProc restore request.
PBZFM_ShutdownFn g_shutdownFn = nullptr;
HANDLE g_shutdownThread = nullptr;
int g_shutdownPolls = 0;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    printf("[host] ");
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

LRESULT CALLBACK HostWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            HBRUSH brush = CreateSolidBrush(RGB(28, 34, 48));
            FillRect(dc, &r, brush);
            DeleteObject(brush);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(120, 130, 150));
            DrawTextW(dc, L"PBZFM host window (class: UnrealWindow)", -1, &r,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(h, msg, w, l);
    }
}

void SendKey(WORD vk) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[1].type = INPUT_KEYBOARD;
    in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void SendMouseMove(int sx, int sy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dx = (LONG)((sx - vx) * 65535 / (vw - 1));
    in.mi.dy = (LONG)((sy - vy) * 65535 / (vh - 1));
    SendInput(1, &in, sizeof(INPUT));
}

void SendMouseButton(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

// Human-paced chord: modifier downs, pause, key tap, pause, modifier ups. A
// single SendInput burst would leave the batch tail in flight while the DLL's
// replay fires, and the hook chain is allowed to skip/reorder around a busy
// hook - the phases mirror how a real user holds the modifiers.
void SendChord(const WORD* mods, int nMods, WORD vk) {
    INPUT in[10]{};
    int n = 0;
    for (int i = 0; i < nMods; ++i) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = mods[i]; ++n; }
    SendInput(n, in, sizeof(INPUT));
    Sleep(120);
    n = 0;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; ++n;
    in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = vk; in[n].ki.dwFlags = KEYEVENTF_KEYUP; ++n;
    SendInput(n, in, sizeof(INPUT));
    Sleep(80);
    n = 0;
    for (int i = nMods - 1; i >= 0; --i) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = mods[i]; in[n].ki.dwFlags = KEYEVENTF_KEYUP; ++n; }
    SendInput(n, in, sizeof(INPUT));
}

// The passthrough auto-close needs the host window to be the foreground
// (the hook compares the foreground against the "game" window).
void FocusHost() {
    HWND fg = GetForegroundWindow();
    if (fg == g_hwnd) return;
    DWORD fgTid = GetWindowThreadProcessId(fg, nullptr);
    DWORD myTid = GetCurrentThreadId();
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, TRUE);
    BringWindowToTop(g_hwnd);
    SetForegroundWindow(g_hwnd);
    if (fgTid && fgTid != myTid) AttachThreadInput(myTid, fgTid, FALSE);
}

void SendClickAt(float cx, float cy) {
    if (cx < 0 || cy < 0) {
        Log("SendClickAt skipped (invalid target %.0f,%.0f)", cx, cy);
        return;
    }
    POINT p{ (LONG)cx, (LONG)cy };
    ClientToScreen(g_hwnd, &p);
    Log("click at client %.0f,%.0f -> screen %ld,%ld%s", cx, cy, p.x, p.y,
        g_synthetic ? " (synthetic)" : "");
    if (g_synthetic && pfnInjectMove && pfnInjectButton) {
        pfnInjectMove(p.x, p.y);
        Sleep(120);   // let the render thread consume the position first
        pfnInjectButton(0, true);
        Sleep(60);
        pfnInjectButton(0, false);
        return;
    }
    SendMouseMove(p.x, p.y);
    Sleep(80);
    SendMouseButton(true);
    Sleep(60);
    SendMouseButton(false);
}

bool SaveShot(const char* name) {
    // The host is not DPI-aware: GetClientRect/ClientToScreen are virtualized
    // at 96 DPI while BitBlt reads physical screen pixels, so virtual rects
    // miss the bottom of the panel. DWM frame bounds are physical regardless.
    RECT fr{};
    if (FAILED(DwmGetWindowAttribute(g_hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                     &fr, sizeof(fr)))) {
        GetWindowRect(g_hwnd, &fr);
    }
    POINT origin{ fr.left, fr.top };
    int w = fr.right - fr.left;
    int h = fr.bottom - fr.top;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);
    unsigned char* px = (unsigned char*)bits;
    for (int i = 0; i < w * h; ++i) std::swap(px[i * 4 + 0], px[i * 4 + 2]);
    std::string path = std::string("shots/") + name;
    bool ok = stbi_write_png(path.c_str(), w, h, 4, px, w * 4) != 0;
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    Log("screenshot %s (%s)", name, ok ? "ok" : "FAILED");
    return ok;
}

std::string ReadSmallFile(const char* path) {
    // _SH_DENYNO: PBZFM.log is held open by the DLL while we read it mid-run.
    FILE* f = _fsopen(path, "rb", _SH_DENYNO);
    if (!f) {
        char buf[160];
        snprintf(buf, sizeof(buf), "(read failed: errno=%d winerr=%lu)", errno, GetLastError());
        return buf;
    }
    std::string out;
    char buf[512];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, rd);
    fclose(f);
    return out;
}

std::string ExeDir() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string d = path;
    size_t p = d.find_last_of("\\/");
    return p == std::string::npos ? std::string(".") : d.substr(0, p);
}

int FindSectionByUtf8Name(const char* name) {
    if (!pfnSectionCount || !pfnSectionName) return -1;
    for (int i = 0; i < pfnSectionCount(); ++i) {
        char buf[128] = {};
        pfnSectionName(i, buf, sizeof(buf));
        if (strcmp(buf, name) == 0) return i;
    }
    return -1;
}

void DumpArtifacts() {
    char dir[MAX_PATH]{};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    std::string d = dir;
    size_t p = d.find_last_of("\\/");
    if (p != std::string::npos) d = d.substr(0, p);
    Log("--- demo_mod_state.txt ---\n%s", ReadSmallFile((d + "\\demo_mod_state.txt").c_str()).c_str());
    Log("--- data/demo.json ---\n%s", ReadSmallFile((d + "\\data\\demo.json").c_str()).c_str());
    std::string logPath = d + "\\PBZFM.log"; // fallback: beside the DLL
    PWSTR docs = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) {
        char docsA[MAX_PATH]{};
        WideCharToMultiByte(CP_UTF8, 0, docs, -1, docsA, sizeof(docsA), nullptr, nullptr);
        CoTaskMemFree(docs);
        logPath = std::string(docsA) + "\\PBZ\\Logs\\PBZFM.log";
    }
    Log("--- PBZFM.log (%s) ---\n%s", logPath.c_str(), ReadSmallFile(logPath.c_str()).c_str());
}

void AssertSchemaRoundTrip() {
    char dir[MAX_PATH]{};
    GetModuleFileNameA(nullptr, dir, MAX_PATH);
    std::string d = dir;
    size_t p = d.find_last_of("\\/");
    if (p != std::string::npos) d = d.substr(0, p);
    std::string json = ReadSmallFile((d + "\\data\\demo.json").c_str());
    // B1 regression: values seeded on disk before the mod declared its controls
    // must survive the load and be written back unchanged on menu close.
    static const char* const kNeedles[] = { "\"fov\": 120", "\"quality\": 3", "\"shot_key\": 66" };
    static const char* const kWhat[] = { "fov", "quality", "shot_key" };
    for (int i = 0; i < 3; ++i) {
        bool ok = json.find(kNeedles[i]) != std::string::npos;
        Log("B1 regression: %-8s disk value round-trip %s", kWhat[i], ok ? "PASS" : "FAIL");
        if (!ok) g_fail = 1;
    }
}

void AssertWindowRestored() {
    // B2 regression: PBZFM_Shutdown joins the render thread and restores the
    // host WndProc; a still-subclassed window would mean the restore never ran.
    bool alive = IsWindow(g_hwnd) != FALSE;
    LONG_PTR proc = GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC);
    bool restored = proc == (LONG_PTR)HostWndProc;
    Log("B2 regression: window alive=%d wndproc=%s %s", alive ? 1 : 0,
        restored ? "HostWndProc" : "STILL SUBCLASSED", (alive && restored) ? "PASS" : "FAIL");
    if (!alive || !restored) g_fail = 1;
}

// M1 regression: the store write must either land whole or leave the previous
// file untouched. The failure path is forced by putting a directory on the
// temp name, so CreateFileW(tmp) fails and the old store must survive.
void AssertAtomicWrite() {
    typedef bool (*AtomicWriteFn)(const char*, const char*);
    AtomicWriteFn fn = (AtomicWriteFn)GetProcAddress(g_dll, "PBZFM_TestAtomicWrite");
    if (!fn) {
        Log("M1 regression: PBZFM_TestAtomicWrite missing - SKIP (configure with -DPBZFM_TEST_HOOKS=ON)");
        return;
    }
    std::string dir = ExeDir() + "\\data";
    std::string okPath = dir + "\\atomic_ok.txt";
    std::string failPath = dir + "\\atomic_fail.txt";
    std::string tmpSquat = failPath + ".tmp";
    auto seed = [](const std::string& p, const char* text) {
        FILE* f = nullptr;
        if (fopen_s(&f, p.c_str(), "wb") != 0 || !f) return false;
        fputs(text, f);
        fclose(f);
        return true;
    };
    bool seeded = seed(okPath, "OLD") && seed(failPath, "KEEP");
    bool wrote = seeded && fn(okPath.c_str(), "NEW");
    bool content = ReadSmallFile(okPath.c_str()) == "NEW";
    bool tmpGone = GetFileAttributesA((okPath + ".tmp").c_str()) == INVALID_FILE_ATTRIBUTES;
    // Clear leftovers from an interrupted run first: a stale object on the temp
    // name would make CreateDirectoryA fail with ERROR_ALREADY_EXISTS and skip
    // the failure-path assertion entirely.
    DeleteFileA(tmpSquat.c_str());
    RemoveDirectoryA(tmpSquat.c_str());
    bool blocked = CreateDirectoryA(tmpSquat.c_str(), nullptr) != FALSE;
    DWORD blockErr = blocked ? 0 : GetLastError();
    bool refused = blocked && !fn(failPath.c_str(), "REPLACED");
    bool kept = ReadSmallFile(failPath.c_str()) == "KEEP";
    RemoveDirectoryA(tmpSquat.c_str());
    bool pass = seeded && wrote && content && tmpGone && refused && kept;
    Log("M1 regression: atomic write (write=%d content=%d tmp-removed=%d refused=%d old-kept=%d blocked=%d err=%lu) %s",
        wrote ? 1 : 0, content ? 1 : 0, tmpGone ? 1 : 0, refused ? 1 : 0, kept ? 1 : 0,
        blocked ? 1 : 0, blockErr, pass ? "PASS" : "FAIL");
    if (!pass) g_fail = 1;
}

// M10 regression: DIB pixels carry alpha 0, so the encoder must force alpha to
// 255 before writing the PNG; read it back and inspect the pixel data.
void AssertPngAlpha() {
    typedef bool (*WriteAlphaPngFn)(const char*, int, int);
    WriteAlphaPngFn fn = (WriteAlphaPngFn)GetProcAddress(g_dll, "PBZFM_TestWriteAlphaPng");
    if (!fn) {
        Log("M10 regression: PBZFM_TestWriteAlphaPng missing - SKIP (configure with -DPBZFM_TEST_HOOKS=ON)");
        return;
    }
    std::string path = ExeDir() + "\\shots\\png_alpha_test.png";
    bool wrote = fn(path.c_str(), 16, 16);
    int w = 0, h = 0, comp = 0;
    unsigned char* px = wrote ? stbi_load(path.c_str(), &w, &h, &comp, 4) : nullptr;
    bool opaque = px && w == 16 && h == 16;
    if (opaque) {
        for (int i = 0; i < w * h; ++i) {
            if (px[i * 4 + 3] != 255) { opaque = false; break; }
        }
    }
    stbi_image_free(px);
    Log("M10 regression: PNG alpha (%dx%d, all 255) %s", w, h, opaque ? "PASS" : "FAIL");
    if (!opaque) g_fail = 1;
}

// L5 regression: removing a hotkey must forget its "held" bit, otherwise
// registering the same chord again would swallow the next press.
static int g_hotkeyProbeFires = 0;
static void HotkeyProbe(void*) { ++g_hotkeyProbeFires; }

void AssertHotkeyReregister() {
    typedef int (*AddHotkeyExFn)(unsigned, unsigned, void (*)(void*), void*);
    typedef void (*RemoveHotkeyFn)(int);
    AddHotkeyExFn add = (AddHotkeyExFn)GetProcAddress(g_dll, "PBZFM_AddHotkeyEx");
    RemoveHotkeyFn rem = (RemoveHotkeyFn)GetProcAddress(g_dll, "PBZFM_RemoveHotkey");
    if (!add || !rem || !pfnDispatchHotkey) {
        Log("L5 regression: hotkey exports missing - SKIP (configure with -DPBZFM_TEST_HOOKS=ON)");
        return;
    }
    const unsigned kVk = VK_F10;   // demo mod uses F9; F10 is free in the host scenario
    g_hotkeyProbeFires = 0;
    int id = add(kVk, 0, HotkeyProbe, nullptr);
    bool first = id != 0 && pfnDispatchHotkey(kVk) && g_hotkeyProbeFires == 1;
    rem(id);
    int again = add(kVk, 0, HotkeyProbe, nullptr);
    bool second = again != 0 && pfnDispatchHotkey(kVk) && g_hotkeyProbeFires == 2;
    rem(again);
    bool pass = first && second;
    Log("L5 regression: hotkey re-register (first press=%d after re-register=%d) %s",
        first ? 1 : 0, second ? 1 : 0, pass ? "PASS" : "FAIL");
    if (!pass) g_fail = 1;
}

DWORD WINAPI ShutdownThread(LPVOID) {
    if (g_shutdownFn) g_shutdownFn();
    return 0;
}

void Finish() {
    AssertSchemaRoundTrip();
    DumpArtifacts();
    if (g_demo) { FreeLibrary(g_demo); g_demo = nullptr; }
    if (g_dll) {
        g_shutdownFn = (PBZFM_ShutdownFn)GetProcAddress(g_dll, "PBZFM_Shutdown");
        if (g_shutdownFn) {
            // Do not call it inline: PBZFM_Shutdown joins the init thread,
            // which posts the WndProc restore to this window thread. Blocking
            // this thread would make that restore time out (B2's exact path).
            g_shutdownThread = CreateThread(nullptr, 0, ShutdownThread, nullptr, 0, nullptr);
            if (g_shutdownThread) {
                SetTimer(g_hwnd, 19, 200, nullptr);
                return;   // keep pumping; timer 19 finishes the run
            }
            Log("ERROR: shutdown thread creation failed - calling inline");
            g_shutdownFn();
        }
        // Verify the WndProc restore while PBZFM.dll is still mapped - its
        // original proc lives in that module.
        AssertWindowRestored();
        FreeLibrary(g_dll);
        g_dll = nullptr;
    }
    PostQuitMessage(0);
}

} // namespace

int main(int argc, char** argv) {
    g_synthetic = argc > 1 && strcmp(argv[1], "--synthetic") == 0;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kHostClass;
    if (!RegisterClassExW(&wc)) {
        Log("RegisterClassEx failed err=%lu", GetLastError());
        return 1;
    }
    g_hwnd = CreateWindowExW(0, kHostClass, L"PBZFM Host", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             120, 100, kW, kH, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hwnd) {
        Log("CreateWindow failed err=%lu", GetLastError());
        return 1;
    }

    char selfPath[MAX_PATH]{};
    GetModuleFileNameA(nullptr, selfPath, MAX_PATH);
    std::string dir = selfPath;
    size_t p = dir.find_last_of("\\/");
    if (p != std::string::npos) dir = dir.substr(0, p);
    CreateDirectoryA("shots", nullptr);
    SetCurrentDirectoryA(dir.c_str());
    CreateDirectoryA("shots", nullptr);
    DeleteFileA("demo_mod_state.txt");

    // B1 regression seed: values already on disk when the mod declares its
    // controls. If the framework applies them (two-phase load), the close-time
    // save writes them back; AssertSchemaRoundTrip() checks that later.
    CreateDirectoryA("data", nullptr);
    {
        FILE* f = nullptr;
        if (fopen_s(&f, "data\\demo.json", "w") == 0 && f) {
            fputs("{\n  \"enable_fx\": true,\n  \"fov\": 120,\n  \"quality\": 3,\n  \"shot_key\": 66\n}\n", f);
            fclose(f);
            Log("B1 regression: seeded data/demo.json (fov=120 quality=3 shot_key=66)");
        } else {
            Log("B1 regression: seed write FAILED");
            g_fail = 1;
        }
    }

    g_dll = LoadLibraryW(L"PBZFM.dll");
    if (!g_dll) {
        Log("LoadLibrary(PBZFM.dll) failed err=%lu", GetLastError());
        return 2;
    }
    auto versionFn = (PBZFM_VersionFn)GetProcAddress(g_dll, "PBZFM_Version");
    Log("PBZFM.dll loaded, version: %s", versionFn ? versionFn() : "(no export)");

    g_demo = LoadLibraryW(L"demo_mod.dll");
    Log("demo_mod.dll %s", g_demo ? "loaded" : "FAILED to load");

    pfnSectionCount = (PBZFM_pfn_int)GetProcAddress(g_dll, "PBZFM_GetSectionCount");
    pfnSectionName = (PBZFM_pfn_GetSectionName)GetProcAddress(g_dll, "PBZFM_GetSectionNameUtf8");
    pfnSectionRect = (PBZFM_pfn_GetRect1)GetProcAddress(g_dll, "PBZFM_GetSectionRect");
    pfnLastSchemaRect = (PBZFM_pfn_GetRect0)GetProcAddress(g_dll, "PBZFM_GetLastSchemaItemRect");
    pfnInjectMove = (PBZFM_TestInjectMoveFn)GetProcAddress(g_dll, "PBZFM_TestInjectMove");
    pfnInjectButton = (PBZFM_TestInjectButtonFn)GetProcAddress(g_dll, "PBZFM_TestInjectButton");
    pfnToggleMenu = (PBZFM_TestToggleMenuFn)GetProcAddress(g_dll, "PBZFM_TestToggleMenu");
    pfnDispatchHotkey = (PBZFM_TestDispatchHotkeyFn)GetProcAddress(g_dll, "PBZFM_TestDispatchHotkey");
    pfnSetPassthrough = (PBZFM_TestSetPassthroughFn)GetProcAddress(g_dll, "PBZFM_TestSetPassthrough");
    pfnPtActive = (PBZFM_TestPtActiveFn)GetProcAddress(g_dll, "PBZFM_TestPassthroughActive");

    // M8: --synthetic drives the DLL only through test exports, so a release
    // build (PBZFM_TEST_HOOKS=OFF) cannot run this scenario - say so instead of
    // silently timing out.
    if (g_synthetic && (!pfnToggleMenu || !pfnInjectMove || !pfnInjectButton)) {
        Log("ERROR: --synthetic needs a PBZFM_TEST_HOOKS=ON build (test exports missing)");
        return 3;
    }
    AssertAtomicWrite();
    AssertPngAlpha();
    AssertHotkeyReregister();

    SetTimer(g_hwnd, 1, 900, nullptr);    // shot 00_initial
    SetTimer(g_hwnd, 2, 1500, nullptr);   // F8 -> menu
    SetTimer(g_hwnd, 3, 2800, nullptr);   // click demo schema section
    SetTimer(g_hwnd, 4, 3400, nullptr);   // shot 01_schema_page
    SetTimer(g_hwnd, 5, 4000, nullptr);   // shot 02_schema_page_stable
    SetTimer(g_hwnd, 6, 4600, nullptr);   // click custom section
    SetTimer(g_hwnd, 7, 5200, nullptr);   // click demo button (queried)
    SetTimer(g_hwnd, 8, 5800, nullptr);   // shot 03_custom_page
    SetTimer(g_hwnd, 9, 6400, nullptr);   // shot 03b
    SetTimer(g_hwnd, 13, 6900, nullptr);  // click footer settings button (bottom-left)
    SetTimer(g_hwnd, 14, 7700, nullptr);  // shot 05_settings_page
    SetTimer(g_hwnd, 10, 8400, nullptr);  // F8 close
    SetTimer(g_hwnd, 11, 9600, nullptr);  // F9 demo hotkey
    SetTimer(g_hwnd, 15, 9800, nullptr);  // F8 open (passthrough scenario, real input only)
    SetTimer(g_hwnd, 16, 10600, nullptr); // Ctrl+Alt+F9 chord -> borrow window opens
    SetTimer(g_hwnd, 17, 12200, nullptr); // grace (1 s) elapsed + move -> isolation resumed
    SetTimer(g_hwnd, 18, 12600, nullptr); // F8 close
    SetTimer(g_hwnd, 12, 13500, nullptr); // shot 04_closed + finish

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (msg.message == WM_TIMER && msg.hwnd == g_hwnd) {
            KillTimer(g_hwnd, (UINT)msg.wParam);
            switch (msg.wParam) {
                case 1: SaveShot("00_initial.png"); break;
                case 2:
                    Log("F8 (open)");
                    if (g_synthetic && pfnToggleMenu) pfnToggleMenu(true); else SendKey(VK_F8);
                    break;
                case 3: {
                    int idx = -1;
                    for (int t = 0; t < 30 && idx < 0; ++t) {
                        idx = FindSectionByUtf8Name("\xe6\xbc\x94\xe7\xa4\xba\xe6\xa8\xa1\xe7\xbb\x84");
                        if (idx < 0) Sleep(100);
                    }
                    float r[4] = { -1, -1, -1, -1 };
                    for (int t = 0; t < 30 && !pfnSectionRect(idx, r); ++t) Sleep(100);
                    Log("demo schema section idx=%d rect=(%.0f,%.0f)-(%.0f,%.0f)",
                        idx, r[0], r[1], r[2], r[3]);
                    SendClickAt((r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f);
                    break;
                }
                case 4: SaveShot("01_schema_page.png"); break;
                case 5: SaveShot("02_schema_page_stable.png"); break;
                case 6: {
                    int idx = -1;
                    float r[4] = { -1, -1, -1, -1 };
                    for (int t = 0; t < 30 && (idx < 0 || r[0] < 0); ++t) {
                        idx = FindSectionByUtf8Name("\xe6\xbc\x94\xe7\xa4\xba\xe6\xa8\xa1\xe7\xbb\x84 \xc2\xb7 \xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89");
                        if (idx >= 0 && pfnSectionRect) pfnSectionRect(idx, r);
                        if (idx < 0 || r[0] < 0) Sleep(100);
                    }
                    Log("custom section idx=%d rect=(%.0f,%.0f)-(%.0f,%.0f)",
                        idx, r[0], r[1], r[2], r[3]);
                    SendClickAt((r[0] + r[2]) * 0.5f, (r[1] + r[3]) * 0.5f);
                    break;
                }
                case 7: {
                    auto getBtn = (PBZFMDEMO_GetButtonFn)GetProcAddress(g_demo, "PBZFMDEMO_GetButton");
                    long bx = -1, by = -1;
                    if (getBtn) {
                        for (int i = 0; i < 40 && !getBtn(&bx, &by); ++i) Sleep(50);
                    }
                    if (bx >= 0) {
                        Log("demo button client %ld,%ld (queried)", bx, by);
                        SendClickAt((float)bx, (float)by);
                    } else {
                        Log("demo button position unavailable (page not rendered?)");
                    }
                    break;
                }
                case 8: SaveShot("03_custom_page.png"); break;
                case 13:
                    Log("click settings button at %.0f,%.0f",
                        pbzfm::layout::kSettingsBtnCx, pbzfm::layout::kSettingsBtnCy);
                    SendClickAt(pbzfm::layout::kSettingsBtnCx, pbzfm::layout::kSettingsBtnCy);
                    break;
                case 14: SaveShot("05_settings_page.png"); break;
                case 10:
                    Log("F8 (close)");
                    if (g_synthetic && pfnToggleMenu) pfnToggleMenu(false); else SendKey(VK_F8);
                    break;
                case 11:
                    Log("F9 (demo hotkey, menu closed)");
                    if (g_synthetic && pfnDispatchHotkey) {
                        Log("hotkey dispatch -> %s", pfnDispatchHotkey(VK_F9) ? "consumed" : "not registered");
                    } else {
                        SendKey(VK_F9);
                    }
                    break;
                case 15:
                    if (g_synthetic) break;   // the synthetic channel bypasses the hooks
                    Log("F8 (open, passthrough scenario)");
                    SendKey(VK_F8);
                    break;
                case 16: {
                    if (g_synthetic || !pfnSetPassthrough || !pfnPtActive) break;
                    FocusHost();
                    // Ctrl+Alt+F9: deliberately not a real tool's hotkey, so the
                    // run never pops a live PixPin/NVIDIA capture overlay.
                    pfnSetPassthrough(VK_F9, true, false, true, false, true);   // exercise the replay path
                    Sleep(100);
                    WORD mods[2] = { VK_CONTROL, VK_MENU };
                    SendChord(mods, 2, VK_F9);
                    Sleep(250);   // covers the paced chord (~200 ms) + probe
                    int st = pfnPtActive();
                    Log("passthrough after Ctrl+Alt+F9: %d (expect 1)%s", st, st == 1 ? "" : "  <- FAILED");
                    if (st != 1) g_fail = 1;
                    break;
                }
                case 17: {
                    if (g_synthetic || !pfnPtActive) break;
                    SendMouseMove(kW / 2, kH / 2);
                    Sleep(300);
                    int st = pfnPtActive();
                    bool fgHost = GetForegroundWindow() == g_hwnd;
                    Log("passthrough after grace+move: %d (expect 0)%s", st,
                        !fgHost ? "  (fg not on host: check skipped)" : (st == 0 ? "" : "  <- FAILED"));
                    if (fgHost && st != 0) g_fail = 1;
                    break;
                }
                case 18:
                    if (g_synthetic) break;
                    Log("F8 (close, passthrough scenario)");
                    SendKey(VK_F8);
                    break;
                case 19: {
                    g_shutdownPolls++;
                    if (WaitForSingleObject(g_shutdownThread, 0) == WAIT_OBJECT_0) {
                        CloseHandle(g_shutdownThread);
                        g_shutdownThread = nullptr;
                        AssertWindowRestored();
                        FreeLibrary(g_dll);
                        g_dll = nullptr;
                        PostQuitMessage(0);
                        break;
                    }
                    if (g_shutdownPolls >= 30) {   // ~6 s: report instead of hanging
                        Log("ERROR: PBZFM_Shutdown did not finish within 6s");
                        g_fail = 1;
                        PostQuitMessage(0);   // do not free a DLL with a live init thread
                        break;
                    }
                    SetTimer(g_hwnd, 19, 200, nullptr);
                    break;
                }
                case 12:
                    SaveShot("04_closed.png");
                    Finish();
                    break;
            }
        }
    }
    Log("host done, fail=%d", g_fail);
    return g_fail ? 1 : 0;
}
