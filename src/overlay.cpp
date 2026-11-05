// DirectComposition overlay: own top-level window + own D3D11 device +
// swapchain created with CreateSwapChainForComposition. Nothing here touches
// the game's swap chain or D3D objects, so FSR3 frame generation / Streamline
// (which killed the Present-hook route, see PBZPhotoMode PROGRESS 1.1.x) is
// unaffected. Graphics DLLs are loaded at runtime, after the game window
// exists, so the PE import table stays clean while UE4SS loads us early.
#include "overlay.h"

#include "common.h"
#include "config.h"
#include "engine_port.h"
#include "i18n.h"
#include "game_window.h"
#include "input.h"
#include "menu.h"
#include "layout.h"
#include "registry.h"
#include "schema.h"
#include "textures.h"
#include "theme.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

using Microsoft::WRL::ComPtr;

namespace pbzfm {

namespace {

typedef HRESULT(WINAPI* PFN_D3D11CreateDevice)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT(WINAPI* PFN_CreateDXGIFactory2)(UINT Flags, REFIID riid, void** ppFactory);
typedef HRESULT(WINAPI* PFN_DCompositionCreateDevice)(IDXGIDevice* pDxgiDevice, REFIID riid, void** ppDevice);
typedef HRESULT(WINAPI* PFN_D3DCompile)(LPCVOID pSrcData, SIZE_T SrcDataSize, LPCSTR pSourceName,
    const D3D_SHADER_MACRO* pDefines, ID3DInclude* pInclude, LPCSTR pEntrypoint, LPCSTR pTarget,
    UINT Flags1, UINT Flags2, ID3DBlob** ppCode, ID3DBlob** ppErrorMsgs);

constexpr wchar_t kOverlayClassBase[] = L"PBZFMOverlayWnd";

// H1: raw-input ownership state machine. `rawList` is only meaningful in the
// StolenWithGameEntry state (the game's exact registrations captured before we
// re-pointed the mouse entry at our overlay).
enum RawMouseState {
    kRawNotStolen,          // nothing of ours registered; restore is a no-op
    kRawStolenWithGameEntry,// our sink replaced a game mouse entry; rawList must be put back
    kRawStealFailed,         // RegisterRawInputDevices failed; the game's entry is untouched
    kRawStolenOnlyOurs       // no game mouse entry existed; remove only our sink on close
};

struct Overlay {
    HWND hwnd = nullptr;
    bool visible = false;
    int  w = 0, h = 0;
    // H7: the overlay window class is registered under THIS DLL's module with a
    // process/time-unique name, so a hot-reloaded DLL never inherits the atom of
    // a previous instance (whose WndProc may already be unmapped).
    std::wstring className;
    HINSTANCE classInstance = nullptr;
    bool classRegistered = false;

    ComPtr<ID3D11Device> dev;
    ComPtr<ID3D11DeviceContext> ctx;
    ComPtr<IDXGISwapChain1> sc;
    ComPtr<ID3D11RenderTargetView> rtv;   // flip model: GetBuffer(0) is always the current back buffer
    ComPtr<ID3D11VertexShader> fsVs;
    ComPtr<ID3D11PixelShader> fsPs;
    ComPtr<ID3D11BlendState> premulBs;
    ComPtr<ID3D11RasterizerState> rs;

    ComPtr<IDCompositionDevice> ddev;
    ComPtr<IDCompositionTarget> dtarget;
    ComPtr<IDCompositionVisual> dvisual;

    // M11: the graphics DLL handles are cached for the process lifetime: a
    // rebuild reuses them instead of loading d3dcompiler a second time, and
    // CleanupGraphics releases them once.
    HMODULE hD3d11 = nullptr;
    HMODULE hDxgi = nullptr;
    HMODULE hDcomp = nullptr;
    HMODULE hD3dc = nullptr;

    bool imguiReady = false;
    // M12: device-removed/reset requests a rebuild on the next frame; if that
    // also fails the overlay degrades to inert instead of drawing into a dead
    // device or a null RTV.
    bool rebuildPending = false;
    bool graphicsLost = false;
    bool cursorHiddenByUs = false;  // we hid a previously visible cursor, restore on close
    bool mousePosValid = false;     // ImGui mouse position known since menu open
    bool cursorVerifyPending = false;   // log the actual cursor state ~1s after restore
    LARGE_INTEGER cursorVerifyAt{};
    LARGE_INTEGER qpcNow{};
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER qpcLast{};
    LARGE_INTEGER diagNext{};           // 1 Hz input/present diagnostics while the menu is open
    float presentMaxMs = 0.0f;
    bool gameWasFg = false;            // foreground edge detector for the reclaim watchdog
    LARGE_INTEGER rawWatchNext{};      // periodic raw-input ownership check (4 Hz)
    std::vector<RAWINPUTDEVICE> rawList;   // game's raw-input registrations, saved on open
    RawMouseState rawState = kRawNotStolen;

    GameWindow* gw = nullptr;
    Config* cfg = nullptr;
    I18n* i18n = nullptr;
    std::wstring iniPath;
    MenuUi ui;
    HANDLE thread = nullptr;
};

Overlay g_ov;

LRESULT CALLBACK OverlayWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_ERASEBKGND) return 1;
    return DefWindowProcW(h, msg, w, l);
}

// The game's camera runs on raw mouse input, and on this Windows build raw
// input survives LL-hook blocking - eating the moves is not enough. Raw input
// registration is process-wide, one entry per device: registering re-points
// the delivery. So on menu open the mouse entry is re-pointed at OUR overlay
// (INPUTSINK) and the game's exact saved list is restored on close - never
// RIDEV_REMOVE (that leaves the game without its registration and its camera
// dead until restart, verified in-game 2026-11-01).
bool RawMouseIsOurs();   // defined below; the steal checks ownership first

bool StealGameRawMouse() {
    // H1: never save an already-stolen list as "the game's". If our sink is
    // still current, re-stealing would capture OUR entry into rawList and the
    // close-time restore would re-register our own sink instead of the game's
    // (camera dead until restart).
    if (RawMouseIsOurs()) {
        Log("[ov] raw mouse already points at the overlay - steal skipped");
        return true;
    }

    UINT n = 0;
    GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
    std::vector<RAWINPUTDEVICE> saved(n);
    UINT got = n;
    if (n > 0 && GetRegisteredRawInputDevices(saved.data(), &got, sizeof(RAWINPUTDEVICE)) == 0) {
        Log("[ov] ERROR: raw input query failed err=%lu - not stealing (game input untouched)",
            GetLastError());
        g_ov.rawState = kRawStealFailed;
        return false;
    }
    saved.resize(got);

    RAWINPUTDEVICE ours{};
    ours.usUsagePage = 0x01;
    ours.usUsage = 0x02;
    ours.dwFlags = RIDEV_INPUTSINK;   // deliver even though the game keeps focus
    ours.hwndTarget = g_ov.hwnd;

    std::vector<RAWINPUTDEVICE> next;
    bool hadMouse = false;
    for (const RAWINPUTDEVICE& d : saved) {
        if (d.usUsagePage == ours.usUsagePage && d.usUsage == ours.usUsage) {
            next.push_back(ours);   // re-point the game's mouse entry at us
            hadMouse = true;
        } else {
            next.push_back(d);      // keyboard/gamepad entries survive verbatim
        }
    }
    if (!hadMouse) next.push_back(ours);

    if (!RegisterRawInputDevices(next.data(), (UINT)next.size(), sizeof(RAWINPUTDEVICE))) {
        // H1: a failed steal leaves the process-wide registration exactly as it
        // was. Record that so close does NOT run the "remove our sink" branch
        // (which would delete the game's live entry).
        Log("[ov] ERROR: raw mouse re-point failed err=%lu - game input untouched", GetLastError());
        g_ov.rawState = kRawStealFailed;
        return false;
    }
    g_ov.rawList = std::move(saved);
    g_ov.rawState = hadMouse ? kRawStolenWithGameEntry : kRawStolenOnlyOurs;
    Log("[ov] raw mouse re-pointed to overlay (%u entries saved, state=%s)", (unsigned)got,
        hadMouse ? "with-game-entry" : "only-ours");
    return true;
}

void RestoreGameRawMouse() {
    // H1: a failed steal must never trigger a restore - the game's registration
    // was never replaced, so any registration call here could only damage it.
    if (g_ov.rawState == kRawStealFailed) {
        Log("[ov] raw mouse: earlier steal failed - nothing to restore");
        g_ov.rawState = kRawNotStolen;
        return;
    }
    // H1: re-query ownership before transitioning. The game re-registers raw
    // input when it regains focus / reinitialises its input stack, which replaces
    // our entry; calling RIDEV_REMOVE then would destroy the game's fresh
    // registration and freeze its camera until restart.
    if (!RawMouseIsOurs()) {
        if (g_ov.rawState != kRawNotStolen)
            Log("[ov] raw mouse: registration was replaced by the game - nothing to restore");
        g_ov.rawList.clear();
        g_ov.rawState = kRawNotStolen;
        return;
    }
    if (g_ov.rawState == kRawStolenWithGameEntry) {
        if (g_ov.rawList.empty()) {
            // Cannot happen with the steal code above, but never fall through to
            // RIDEV_REMOVE: removing would kill the game's mouse registration.
            Log("[ov] ERROR: restore list empty in stolen state - leaving registrations untouched");
            return;
        }
        if (!RegisterRawInputDevices(g_ov.rawList.data(), (UINT)g_ov.rawList.size(), sizeof(RAWINPUTDEVICE))) {
            // H1: report as an error and keep state/list so a later attempt can
            // still restore; a silent WARNING would hide a dead camera.
            Log("[ov] ERROR: raw input restore failed err=%lu (game camera may stay frozen)",
                GetLastError());
            return;
        }
        Log("[ov] raw input registrations restored to game");
        g_ov.rawList.clear();
        g_ov.rawState = kRawNotStolen;
        return;
    }
    if (g_ov.rawState == kRawStolenOnlyOurs) {
        // Nothing existed before us, so removing our sink returns the process to
        // that exact state. Safe ONLY because RawMouseIsOurs() above confirmed
        // the live entry is still the one we registered.
        RAWINPUTDEVICE rm{};
        rm.usUsagePage = 0x01;
        rm.usUsage = 0x02;
        rm.dwFlags = RIDEV_REMOVE;
        rm.hwndTarget = nullptr;
        if (!RegisterRawInputDevices(&rm, 1, sizeof(rm))) {
            Log("[ov] ERROR: raw sink removal failed err=%lu", GetLastError());
            return;
        }
        Log("[ov] raw mouse sink removed (no game entry existed before the steal)");
        g_ov.rawState = kRawNotStolen;
        return;
    }
    // kRawNotStolen but our hwnd owns the entry: the state was lost (e.g. DLL
    // hot-reloaded mid-session). Leave it alone - see the RIDEV_REMOVE warning.
    Log("[ov] raw mouse: ours is live without a saved state - leaving it untouched");
}

// True while the process-wide mouse raw-input entry still points at our
// overlay. Never re-steal while it returns true: StealGameRawMouse saves the
// list it sees as the restore state, and seeing our own entry there would
// make close-time restore re-register OUR registration instead of the game's
// (camera dead until restart, the exact failure the save/restore exists for).
bool RawMouseIsOurs() {
    // The NULL-buffer count query returns FALSE even when it fills n (probed
    // 2026-11-01): ignore the return and trust n, mirroring StealGameRawMouse.
    UINT n = 0;
    GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
    std::vector<RAWINPUTDEVICE> list(n);
    UINT got = n;
    if (n > 0 && !GetRegisteredRawInputDevices(list.data(), &got, sizeof(RAWINPUTDEVICE))) return false;
    for (const RAWINPUTDEVICE& d : list) {
        if (d.usUsagePage == 0x01 && d.usUsage == 0x02)
            return d.hwndTarget == g_ov.hwnd && (d.dwFlags & RIDEV_INPUTSINK) != 0;
    }
    return false;   // no mouse entry at all - ours is gone, steal again
}

// M10: lives outside the anonymous namespace so the test hook in pbzfm.cpp
// calls the exact encoder the capture path uses; the banner below restores it.
} // namespace

// M10: 32bpp DIB pixels are BGRA and BitBlt leaves the alpha byte at 0, so a
// 4-channel PNG came out fully transparent. Convert to RGBA and force alpha to
// 255 in the same pass; every capture path (and the host's alpha test) goes
// through this one encoder.
bool EncodeOpaqueBgraPng(const unsigned char* bgra, int w, int h, std::vector<unsigned char>& png) {
    if (!bgra || w <= 0 || h <= 0) return false;
    std::vector<unsigned char> rgba((size_t)w * (size_t)h * 4);
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; ++i) {
        rgba[i * 4 + 0] = bgra[i * 4 + 2];
        rgba[i * 4 + 1] = bgra[i * 4 + 1];
        rgba[i * 4 + 2] = bgra[i * 4 + 0];
        rgba[i * 4 + 3] = 255;   // DIB alpha is 0, not a real transparency value
    }
    png.clear();
    png.reserve(rgba.size());
    stbi_write_png_to_func([](void* ctx, void* data, int size) {
        auto* out = (std::vector<unsigned char>*)ctx;
        const auto* p = (const unsigned char*)data;
        out->insert(out->end(), p, p + size);
    }, &png, w, h, 4, rgba.data(), w * 4);
    return !png.empty();
}

namespace {

// Full composited-desktop capture of the game window region: DWM hands us the
// final image, so the menu overlay itself is in the shot - N卡/PixPin style
// global tools cannot run while the menu owns the input, this is the escape.
static void CaptureGameScreen() {
    GameWindow* gw = g_ov.gw;
    if (!gw || gw->w <= 0 || gw->h <= 0) return;

    std::vector<unsigned char> png;
    bool ok = false;
    HDC screen = GetDC(nullptr);
    if (screen) {
        HDC mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = gw->w;
        bi.bmiHeader.biHeight = -gw->h;   // top-down rows
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp && bits) {
            HGDIOBJ old = SelectObject(mem, bmp);
            BitBlt(mem, 0, 0, gw->w, gw->h, screen, gw->x, gw->y, SRCCOPY);
            SelectObject(mem, old);
            ok = EncodeOpaqueBgraPng((const unsigned char*)bits, gw->w, gw->h, png);
        }
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
    if (!ok) {
        Log("[ov] screenshot FAILED (%dx%d)", gw->w, gw->h);
        return;
    }

    std::wstring dir = ModRoot() + L"\\Screenshots";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t wide[MAX_PATH]{};
    swprintf(wide, MAX_PATH, L"%s\\PBZFM_%04u%02u%02u_%02u%02u%02u.png", dir.c_str(),
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::ofstream file(wide, std::ios::binary);   // wide path: immune to the ANSI codepage
    file.write((const char*)png.data(), (std::streamsize)png.size());
    ok = (bool)file;
    Log("[ov] screenshot %ls (%dx%d, %zu bytes)", wide, gw->w, gw->h, png.size());
}

// H7: window classes must be registered under the DLL's own module (the game
// exe's HINSTANCE was the old (wrong) owner) and that same handle is required to
// unregister the class at shutdown.
HMODULE OverlayModule() {
    static HMODULE m = [] {
        HMODULE h = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)(uintptr_t)&OverlayModule, &h);
        return h;
    }();
    return m;
}

void UnregisterOverlayWindowClass();   // H7: definition follows CleanupGraphics

bool CreateOverlayWindow() {
    const HMODULE self = OverlayModule();
    if (!self) {
        Log("[ov] failed to resolve own module for the overlay window class");
        return false;
    }
    // H7: a unique class name means a hot-reloaded DLL can never pick up the atom
    // registered by its previous instance (whose OverlayWndProc is unmapped).
    wchar_t cls[96];
    swprintf(cls, 96, L"%s_%08lX_%llX", kOverlayClassBase, (unsigned long)GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    g_ov.className = cls;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = (HINSTANCE)self;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = g_ov.className.c_str();
    if (!RegisterClassExW(&wc)) {
        // H7: refuse rather than reuse a same-named class - it could be owned by
        // someone else or by an older instance of this DLL.
        const DWORD err = GetLastError();
        Log("[ov] ERROR: RegisterClassEx failed err=%lu (class=%ls) - overlay aborted", err, cls);
        g_ov.className.clear();
        return false;
    }
    g_ov.classRegistered = true;
    g_ov.classInstance = (HINSTANCE)self;

    g_ov.w = g_ov.gw->w;
    g_ov.h = g_ov.gw->h;
    g_ov.hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        g_ov.className.c_str(), L"PBZFM Overlay", WS_POPUP,
        g_ov.gw->x, g_ov.gw->y, g_ov.w, g_ov.h,
        nullptr, nullptr, (HINSTANCE)self, nullptr);
    if (!g_ov.hwnd) {
        Log("[ov] CreateWindowEx failed err=%lu", GetLastError());
        // The class atom stays until OverlayStop/cleanup; leaving it registered
        // is harmless (unique name) but we still drop it when no window uses it.
        UnregisterOverlayWindowClass();
        return false;
    }
    Log("[ov] overlay window created (%dx%d at %d,%d, class=%ls)", g_ov.w, g_ov.h, g_ov.gw->x, g_ov.gw->y, cls);
    return true;
}

bool LoadGraphicsApis(PFN_D3D11CreateDevice* d3dCreate, PFN_CreateDXGIFactory2* factory2,
                      PFN_DCompositionCreateDevice* dcompCreate, PFN_D3DCompile* d3dCompile) {
    // M11: bare LoadLibraryW resolves against the game directory first, so a
    // planted d3d11.dll could win; System32-only loading pins the OS copy.
    // Handles are cached in g_ov and reused by a device rebuild, then freed in
    // CleanupGraphics (the old code leaked every handle and loaded d3dcompiler
    // twice).
    wchar_t sysDir[MAX_PATH]{};
    if (!GetSystemDirectoryW(sysDir, MAX_PATH)) {
        Log("[ov] GetSystemDirectoryW failed err=%lu", GetLastError());
        return false;
    }
    auto load = [&](HMODULE& slot, const wchar_t* name) {
        if (slot) return true;   // already loaded by a previous (re)build
        std::wstring path = std::wstring(sysDir) + L"\\" + name;
        slot = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!slot) Log("[ov] load %ls failed err=%lu", path.c_str(), GetLastError());
        return slot != nullptr;
    };
    if (!load(g_ov.hD3d11, L"d3d11.dll") || !load(g_ov.hDxgi, L"dxgi.dll") ||
        !load(g_ov.hDcomp, L"dcomp.dll") || !load(g_ov.hD3dc, L"d3dcompiler_47.dll")) {
        return false;
    }
    *d3dCreate = (PFN_D3D11CreateDevice)GetProcAddress(g_ov.hD3d11, "D3D11CreateDevice");
    *factory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_ov.hDxgi, "CreateDXGIFactory2");
    *dcompCreate = (PFN_DCompositionCreateDevice)GetProcAddress(g_ov.hDcomp, "DCompositionCreateDevice");
    *d3dCompile = (PFN_D3DCompile)GetProcAddress(g_ov.hD3dc, "D3DCompile");
    if (!*d3dCreate || !*factory2 || !*dcompCreate || !*d3dCompile) {
        Log("[ov] graphics proc resolve failed");
        return false;
    }
    return true;
}

// M12: a removed/reset device reports through these HRESULTs; any other failure
// is a per-frame hiccup that must not trigger a full rebuild.
bool IsDeviceLossHr(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET;
}

// Flip model: buffer 0 always refers to the current back buffer, so one RTV
// serves every frame. Retry lazily if the swap chain was just resized.
bool EnsureRtv() {
    if (g_ov.rtv) return true;
    ComPtr<ID3D11Texture2D> tex;
    HRESULT hr = g_ov.sc->GetBuffer(0, IID_PPV_ARGS(tex.GetAddressOf()));
    if (FAILED(hr)) {
        Log("[ov] GetBuffer(0) failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    hr = g_ov.dev->CreateRenderTargetView(tex.Get(), nullptr, g_ov.rtv.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        Log("[ov] CreateRenderTargetView failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    return true;
}

bool ResizeSwapchain(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    g_ov.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    g_ov.rtv.Reset();
    HRESULT hr = g_ov.sc->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        Log("[ov] ResizeBuffers(%d,%d) failed hr=0x%08lX", w, h, (unsigned long)hr);
        return false;
    }
    g_ov.w = w;
    g_ov.h = h;
    g_ov.rtv.Reset();
    EnsureRtv();
    Log("[ov] swapchain resized to %dx%d", w, h);
    return true;
}

bool InitD3DAndComposition() {
    PFN_D3D11CreateDevice d3dCreate = nullptr;
    PFN_CreateDXGIFactory2 factory2 = nullptr;
    PFN_DCompositionCreateDevice dcompCreate = nullptr;
    PFN_D3DCompile d3dCompile = nullptr;
    if (!LoadGraphicsApis(&d3dCreate, &factory2, &dcompCreate, &d3dCompile)) return false;

    D3D_FEATURE_LEVEL fl{};
    HRESULT hr = d3dCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                           D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                           g_ov.dev.GetAddressOf(), &fl, g_ov.ctx.GetAddressOf());
    if (FAILED(hr)) {
        Log("[ov] D3D11CreateDevice failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    Log("[ov] D3D11 device ready (feature level 0x%04X)", (unsigned)fl);

    ComPtr<IDXGIFactory2> factory;
    hr = factory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        Log("[ov] CreateDXGIFactory2 failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = (UINT)g_ov.w;
    sd.Height = (UINT)g_ov.h;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = factory->CreateSwapChainForComposition(g_ov.dev.Get(), &sd, nullptr, g_ov.sc.GetAddressOf());
    if (FAILED(hr)) {
        Log("[ov] CreateSwapChainForComposition failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    ComPtr<IDXGIDevice> dxgiDev;
    g_ov.dev.As(&dxgiDev);
    hr = dcompCreate(dxgiDev.Get(), IID_PPV_ARGS(g_ov.ddev.GetAddressOf()));
    if (FAILED(hr)) {
        Log("[ov] DCompositionCreateDevice failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    hr = g_ov.ddev->CreateTargetForHwnd(g_ov.hwnd, TRUE, g_ov.dtarget.GetAddressOf());
    if (FAILED(hr)) {
        Log("[ov] CreateTargetForHwnd failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    hr = g_ov.ddev->CreateVisual(g_ov.dvisual.GetAddressOf());
    if (SUCCEEDED(hr)) hr = g_ov.dvisual->SetContent(g_ov.sc.Get());
    if (SUCCEEDED(hr)) hr = g_ov.dtarget->SetRoot(g_ov.dvisual.Get());
    if (SUCCEEDED(hr)) hr = g_ov.ddev->Commit();
    if (FAILED(hr)) {
        Log("[ov] dcomp visual tree failed hr=0x%08lX", (unsigned long)hr);
        return false;
    }
    EnsureRtv();
    Log("[ov] DirectComposition target ready (composition swapchain)");
    return true;
}

bool InitPremulPipeline() {
    // M11: reuse the handle LoadGraphicsApis cached; loading d3dcompiler here
    // again leaked a second reference on every (re)build.
    if (!g_ov.hD3dc) return false;
    PFN_D3DCompile d3dCompile = (PFN_D3DCompile)GetProcAddress(g_ov.hD3dc, "D3DCompile");
    if (!d3dCompile) return false;

    // Fullscreen triangle in the premultiply blend pass: out.rgb = dest.rgb * dest.a
    // (blend factors ZERO / DEST_ALPHA), out.a = dest.a (ZERO / ONE).
    const char* vsSrc = R"SHADER(
struct VSOut { float4 pos : SV_POSITION; };
VSOut main(uint vid : SV_VertexID) {
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    return o;
})SHADER";
    const char* psSrc = "float4 main() : SV_Target { return 0; }";

    ComPtr<ID3DBlob> code, err;
    HRESULT hr = d3dCompile(vsSrc, strlen(vsSrc), nullptr, nullptr, nullptr, "main", "vs_5_0", 0, 0, code.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) { Log("[ov] premul VS compile failed hr=0x%08lX", (unsigned long)hr); return false; }
    hr = g_ov.dev->CreateVertexShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g_ov.fsVs.GetAddressOf());
    if (FAILED(hr)) return false;
    code.Reset();
    hr = d3dCompile(psSrc, strlen(psSrc), nullptr, nullptr, nullptr, "main", "ps_5_0", 0, 0, code.GetAddressOf(), err.ReleaseAndGetAddressOf());
    if (FAILED(hr)) { Log("[ov] premul PS compile failed hr=0x%08lX", (unsigned long)hr); return false; }
    hr = g_ov.dev->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, g_ov.fsPs.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_DEST_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = g_ov.dev->CreateBlendState(&bd, g_ov.premulBs.GetAddressOf());
    if (FAILED(hr)) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    hr = g_ov.dev->CreateRasterizerState(&rd, g_ov.rs.GetAddressOf());
    return SUCCEEDED(hr);
}

bool InitImGui() {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.BackendPlatformName = "pbzfm_dc_overlay";
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    // M5: the font used to be chosen once at startup, so switching to ko-KR at
    // runtime kept YaHei and Hangul rendered as boxes. Load the primary font for
    // the STARTUP language, then MERGE the other script's font into the same
    // atlas: ImGui 1.92 loads glyphs on demand (RendererHasTextures), so a live
    // language switch needs neither an atlas rebuild nor a new texture.
    static const char* const kYahei = "C:/Windows/Fonts/msyh.ttc";
    static const char* const kMalgun = "C:/Windows/Fonts/malgun.ttf";
    static const char* const kFallback = "C:/Windows/Fonts/segoeui.ttf";
    static const char* const kYaheiBold = "C:/Windows/Fonts/msyhbd.ttc";
    static const char* const kMalgunBold = "C:/Windows/Fonts/malgunbd.ttf";
    const bool koreanUi = g_ov.cfg && g_ov.cfg->language == "ko-KR";
    const char* primary = koreanUi ? kMalgun : kYahei;
    const char* primaryBold = koreanUi ? kMalgunBold : kYaheiBold;
    const char* other = koreanUi ? kYahei : kMalgun;   // merged for the other script
    const char* base = nullptr;
    if (io.Fonts->AddFontFromFileTTF(primary, 18.0f)) base = primary;
    else if (io.Fonts->AddFontFromFileTTF(primaryBold, 18.0f)) base = primaryBold;
    else if (io.Fonts->AddFontFromFileTTF(other, 18.0f)) base = other;
    else if (io.Fonts->AddFontFromFileTTF(kFallback, 18.0f)) base = kFallback;
    if (base) {
        Log("[ov] font loaded: %s", base);
        // Merge the complementary script (skip when the base already is that
        // script, otherwise the atlas would hold the same font twice).
        const char* mergeFont = nullptr;
        if (strcmp(base, kYahei) == 0 || strcmp(base, kYaheiBold) == 0 ||
            strcmp(base, kFallback) == 0) {
            mergeFont = kMalgun;   // Hangul for a ko-KR switch
        } else {
            mergeFont = kYahei;    // kana/CJK ideographs for a zh-CN/ja-JP switch
        }
        ImFontConfig merge;
        merge.MergeMode = true;   // same atlas: every script is one texture
        if (io.Fonts->AddFontFromFileTTF(mergeFont, 18.0f, &merge)) {
            Log("[ov] font merged: %s (runtime language switch keeps glyphs)", mergeFont);
        }
    } else {
        io.Fonts->AddFontDefault();
        Log("[ov] WARNING: no system font found, falling back to ImGui default (CJK will show boxes)");
    }

    if (!ImGui_ImplDX11_Init(g_ov.dev.Get(), g_ov.ctx.Get())) {
        Log("[ov] ImGui_ImplDX11_Init failed");
        return false;
    }

    theme::ApplyPbzTheme(g_ov.cfg ? g_ov.cfg->uiOpacity : 0.94f);
    theme::ApplyThemeByName(g_ov.cfg ? g_ov.cfg->theme : "ink");   // themes\<name>.json overrides

    g_ov.imguiReady = true;
    Log("[ov] ImGui ready (backend dx11, dynamic CJK fonts)");
    return true;
}

void SetVisible(bool v);   // defined below RenderFrame; used by device rebuild

void ResetD3DObjects() {
    g_ov.rtv.Reset();
    g_ov.fsVs.Reset();
    g_ov.fsPs.Reset();
    g_ov.premulBs.Reset();
    g_ov.rs.Reset();
    g_ov.sc.Reset();
    g_ov.dvisual.Reset();
    g_ov.dtarget.Reset();
    g_ov.ddev.Reset();
    g_ov.ctx.Reset();
    g_ov.dev.Reset();
}

// M12: recovery from DXGI_ERROR_DEVICE_REMOVED / DEVICE_RESET. Tears the ImGui
// context + backend down with the dead device (so its textures die with it) and
// rebuilds device, composition, pipeline and backend on the same render thread.
// A failure leaves graphicsLost set: the menu stays inert rather than drawing
// into dead objects or a null RTV.
bool RebuildGraphics(const char* why) {
    Log("[ov] %s - rebuilding the private device", why);
    if (g_ov.visible) SetVisible(false);   // restores raw input/cursor and saves state
    TexturesShutdown();
    if (g_ov.imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_ov.imguiReady = false;
    }
    ResetD3DObjects();
    if (!InitD3DAndComposition()) return false;
    if (!InitPremulPipeline()) return false;
    if (!InitImGui()) return false;
    QueryPerformanceCounter(&g_ov.qpcLast);   // no giant dt on the resumed frame
    Log("[ov] device rebuilt");
    return true;
}

void RenderFrame() {
    if (g_ov.rebuildPending) {
        g_ov.rebuildPending = false;
        if (!RebuildGraphics("device removed/reset")) {
            g_ov.graphicsLost = true;
            Log("[ov] ERROR: device rebuild failed - overlay disabled until restart");
            SetVisible(false);
            return;
        }
    }
    if (!g_ov.rtv && !EnsureRtv()) {
        // M12: never render into a null RTV. A removed device queues a rebuild;
        // any other failure just retries lazily on the next frame.
        if (g_ov.dev && FAILED(g_ov.dev->GetDeviceRemovedReason()))
            g_ov.rebuildPending = true;
        return;
    }

    ImGuiIO& io = ImGui::GetIO();

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - g_ov.qpcLast.QuadPart) / (double)g_ov.qpcFreq.QuadPart);
    g_ov.qpcLast = now;
    dt = std::clamp(dt, 1.0f / 240.0f, 0.1f);

    // Focus watchdog while the menu is open. Win/Alt-Tab away: the hooks pass
    // input through (input.cpp GameHasForeground) and the steal keeps the
    // camera frozen. Back on the game window: reclaim everything the
    // re-activation can have taken - the game re-registers raw input (its
    // camera then tracks the mouse with the menu open, in-game 2026-11-01),
    // re-asserts its cursor clip, and its input stack can re-hook itself
    // ahead of us in the LL chain. The 4 Hz poll covers re-registrations
    // without an activation edge (Steam Input does this at any time).
    bool gameFg = g_ov.gw->hwnd && GetForegroundWindow() == g_ov.gw->hwnd;
    if (gameFg && !g_ov.gameWasFg && !EnginePortActive()) {
        InputRehook();
        g_ov.gw->UnclipCursor();
        bool restolen = !RawMouseIsOurs();
        if (restolen) StealGameRawMouse();
        Log("[ov] game regained foreground - rehook + unclip%s", restolen ? " + raw re-steal" : "");
    }
    g_ov.gameWasFg = gameFg;
    if (!EnginePortActive() && now.QuadPart >= g_ov.rawWatchNext.QuadPart) {
        g_ov.rawWatchNext.QuadPart = now.QuadPart + g_ov.qpcFreq.QuadPart / 4;
        if (!RawMouseIsOurs()) StealGameRawMouse();
    }

    InputEvent ev;
    bool gotPos = false;
    while (InputPop(&ev)) {
        switch (ev.type) {
            case InputEvent::Key: io.AddKeyEvent((ImGuiKey)ev.key, ev.down); break;
            case InputEvent::Btn:
                // B3: never hand ImGui an index outside 0..ImGuiMouseButton_COUNT-1
                // (AddMouseButtonEvent's assert is compiled out in Release and
                // would write past MouseDown[5]). Any source - hooks, test
                // injection, a future backend - is filtered here once.
                if (ev.key < 0 || ev.key >= ImGuiMouseButton_COUNT) {
                    static bool warned = false;
                    if (!warned) {
                        warned = true;
                        Log("[ov] dropped out-of-range mouse button %d (valid 0..%d)", ev.key,
                            ImGuiMouseButton_COUNT - 1);
                    }
                    break;
                }
                io.AddMouseButtonEvent(ev.key, ev.down);
                break;
            case InputEvent::Wheel: io.AddMouseWheelEvent(ev.wx, ev.wy); break;
            case InputEvent::Move: {
                POINT p{ (LONG)ev.wx, (LONG)ev.wy };
                ScreenToClient(g_ov.hwnd, &p);
                io.AddMousePosEvent((float)p.x, (float)p.y);
                gotPos = true;
                break;
            }
        }
    }
    if (!gotPos && !g_ov.mousePosValid) {
        // Seed once per open from the real cursor; afterwards the position
        // comes from the hook's Move events - the real cursor is frozen, so
        // polling it every frame would snap the menu cursor back.
        POINT p{};
        GetCursorPos(&p);
        ScreenToClient(g_ov.hwnd, &p);
        io.AddMousePosEvent((float)p.x, (float)p.y);
    }
    g_ov.mousePosValid = true;

    if (g_inputReset.exchange(0, std::memory_order_acq_rel)) {
        // The passthrough borrow window let keys through to a capture tool;
        // ImGui's idea of the keyboard is stale the moment isolation resumes
        // (modifier-downs fed before the chord, mouse buttons clicked there).
        io.ClearInputKeys();
        // B3: the borrow window also lets XButton down/up reach the tool, so a
        // side-button can be left "down" in ImGui (ClearInputKeys skips mouse
        // keys). Clear the mouse state too; it invalidates MousePos, so reseed
        // the position from the (frozen) real cursor to keep the menu cursor.
        io.ClearInputMouse();
        InputResetDropStats();   // L1: a reset is a new input session for the counters
        POINT mp{};
        GetCursorPos(&mp);
        ScreenToClient(g_ov.hwnd, &mp);
        io.AddMousePosEvent((float)mp.x, (float)mp.y);
    }

    if (now.QuadPart >= g_ov.diagNext.QuadPart) {
        g_ov.diagNext.QuadPart = now.QuadPart + g_ov.qpcFreq.QuadPart;
        POINT rp{};
        GetCursorPos(&rp);
        Log("[ov] diag: hookMoves=%d imgui=(%.0f,%.0f) real=(%ld,%ld) present<=%.1fms",
            g_hookMoves.exchange(0, std::memory_order_relaxed),
            io.MousePos.x, io.MousePos.y, rp.x, rp.y, g_ov.presentMaxMs);
        g_ov.presentMaxMs = 0.0f;
    }
    io.DisplaySize = ImVec2((float)g_ov.w, (float)g_ov.h);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = dt;

    // Key capture result -> dispatch by target (framework toggle or schema key).
    UINT captured = g_capturedVk.exchange(0, std::memory_order_acq_rel);
    if (captured) {
        // H3: the result is written straight into a SchemaItem; hold the registry
        // lock so ConfigAdd*/Get*/Set* from mod threads cannot race the write.
        std::lock_guard<std::recursive_mutex> lock(RegistryMutex());
        int kind = g_captureKind.load(std::memory_order_relaxed);
        if (kind == CAP_SCHEMA_KEY) {
            ConfigDef* c = (ConfigDef*)g_capCfg;
            SchemaItem* it = (SchemaItem*)g_capItem;
            if (c && it) {
                it->ival = captured;
                c->dirty = true;
                SchemaSaveConfigNow(c);
                c->FireChange(it->key.c_str());
                Log("[ov] schema key captured: %s = %s", it->key.c_str(), Config::VkName(captured));
            }
            g_capCfg = nullptr;
            g_capItem = nullptr;
        } else if (kind == CAP_SCHEMA_KEYCOMBO) {
            ConfigDef* c = (ConfigDef*)g_capCfg;
            SchemaItem* it = (SchemaItem*)g_capItem;
            if (c && it) {
                unsigned mods = g_capturedMods.load(std::memory_order_acquire) & 0xFu;
                it->ival = (long long)((captured & 0xFFu) | (mods << 8));
                c->dirty = true;
                SchemaSaveConfigNow(c);
                c->FireChange(it->key.c_str());
                Log("[ov] schema key combo captured: %s = %s", it->key.c_str(),
                    Config::KeyModsToString(captured, mods).c_str());
            }
            g_capCfg = nullptr;
            g_capItem = nullptr;
        } else if (kind == CAP_PASSTHROUGH) {
            // Screenshot-tool passthrough chord captured in the settings page:
            // replace the slot (or append), persist, and hand the new list to
            // the hook immediately - no restart needed.
            unsigned mods = g_capturedMods.load(std::memory_order_acquire) & 0xFu;
            HotkeyChord c{};
            c.vk = captured;
            c.ctrl = (mods & 0x1u) != 0;
            c.shift = (mods & 0x2u) != 0;
            c.alt = (mods & 0x4u) != 0;
            c.win = (mods & 0x8u) != 0;
            auto& keys = g_ov.cfg->passthroughKeys;
            int slot = g_captureIndex.load(std::memory_order_relaxed);
            if (slot >= 0 && slot < (int)keys.size()) {
                keys[slot] = c;
            } else if ((int)keys.size() < 8) {
                keys.push_back(c);
            }
            g_ov.cfg->Save(g_ov.iniPath);
            InputSetPassthrough(keys, g_ov.cfg->passthroughReplay);
            g_ov.ui.saveFlash = 2.0f;
            Log("[ov] passthrough chord captured: %s (slot=%d)", Config::ChordToString(c).c_str(), slot);
        } else {
            g_ov.cfg->toggleVk = captured;
            g_toggleVk.store(captured, std::memory_order_relaxed);
            g_ov.cfg->Save(g_ov.iniPath);
            Log("[ov] toggle key captured: %s (config saved)", Config::VkName(captured));
        }
        g_captureKind.store(CAP_NONE, std::memory_order_relaxed);
    }

    TexturesProcessPending();   // v2 texture uploads + dispose reaping
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        g_closeRequest.store(true, std::memory_order_release);
    }
    DrawMenu(*g_ov.cfg, *g_ov.i18n, g_ov.ui, dt);
    if (g_ov.ui.requestSave) {
        g_ov.ui.requestSave = false;
        g_ov.cfg->Save(g_ov.iniPath);
        Log("[ov] config saved (menu action)");
    }
    if (g_ov.ui.requestScreenshot) {
        g_ov.ui.requestScreenshot = false;
        CaptureGameScreen();
    }

    {
        int cx = g_btnCliX.load(std::memory_order_relaxed);
        int cy = g_btnCliY.load(std::memory_order_relaxed);
        if (cx >= 0) {
            POINT pt{ cx, cy };
            ClientToScreen(g_ov.hwnd, &pt);
            g_btnScrX.store(pt.x, std::memory_order_relaxed);
            g_btnScrY.store(pt.y, std::memory_order_relaxed);
        }
    }

    ImGui::Render();

    ID3D11RenderTargetView* rtv = g_ov.rtv.Get();
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    g_ov.ctx->OMSetRenderTargets(1, &rtv, nullptr);
    g_ov.ctx->ClearRenderTargetView(rtv, clear);
    D3D11_VIEWPORT vp{ 0.0f, 0.0f, (float)g_ov.w, (float)g_ov.h, 0.0f, 1.0f };
    g_ov.ctx->RSSetViewports(1, &vp);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    // premultiply for DirectComposition's premultiplied-alpha surface
    g_ov.ctx->OMSetBlendState(g_ov.premulBs.Get(), nullptr, 0xffffffff);
    g_ov.ctx->RSSetState(g_ov.rs.Get());
    g_ov.ctx->VSSetShader(g_ov.fsVs.Get(), nullptr, 0);
    g_ov.ctx->PSSetShader(g_ov.fsPs.Get(), nullptr, 0);
    g_ov.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ov.ctx->IASetInputLayout(nullptr);
    g_ov.ctx->Draw(3, 0);

    LARGE_INTEGER p0{}, p1{};
    QueryPerformanceCounter(&p0);
    HRESULT hrPresent = g_ov.sc->Present(1, 0);
    HRESULT hrCommit = SUCCEEDED(hrPresent) ? g_ov.ddev->Commit() : hrPresent;
    QueryPerformanceCounter(&p1);
    float pms = (float)((double)(p1.QuadPart - p0.QuadPart) * 1000.0 / (double)g_ov.qpcFreq.QuadPart);
    if (pms > g_ov.presentMaxMs) g_ov.presentMaxMs = pms;
    if (FAILED(hrPresent) || FAILED(hrCommit)) {
        // M12: the old code discarded these HRESULTs and kept drawing on a dead
        // device. A removed/reset device queues a rebuild; other failures are
        // logged at most once a second so a transient hiccup cannot spam.
        bool lost = IsDeviceLossHr(hrPresent) || IsDeviceLossHr(hrCommit) ||
                    (g_ov.dev && FAILED(g_ov.dev->GetDeviceRemovedReason()));
        if (lost) {
            Log("[ov] device lost (present=0x%08lX commit=0x%08lX) - rebuild queued",
                (unsigned long)hrPresent, (unsigned long)hrCommit);
            g_ov.rebuildPending = true;
        } else {
            static LARGE_INTEGER lastLog{};
            if (p1.QuadPart - lastLog.QuadPart >= g_ov.qpcFreq.QuadPart) {
                lastLog = p1;
                Log("[ov] WARNING: present/commit failed (present=0x%08lX commit=0x%08lX)",
                    (unsigned long)hrPresent, (unsigned long)hrCommit);
            }
        }
    }
}

void SetVisible(bool v) {
    if (v == g_ov.visible) return;
    g_ov.visible = v;
    if (v) {
        g_ov.gw->UpdateGeometry();
        if (g_ov.gw->w != g_ov.w || g_ov.gw->h != g_ov.h) {
            ResizeSwapchain(g_ov.gw->w, g_ov.gw->h);
        }
        SetWindowPos(g_ov.hwnd, HWND_TOPMOST, g_ov.gw->x, g_ov.gw->y, g_ov.gw->w, g_ov.gw->h, SWP_NOACTIVATE);
        ShowWindow(g_ov.hwnd, SW_SHOWNA);
        if (EnginePortActive()) {
            // 方案 C：引擎后端负责冻结 look/move 和显示光标（真正的引擎调用
            // 在游戏线程执行）。这里绝不碰 Raw Input、不动 ShowCursor 计数、
            // 不解除游戏的 ClipCursor —— 系统输入流保持原样，录屏/截图热键不受影响。
            EnginePortSetMenuOpen(true);
            g_ov.cursorHiddenByUs = false;
            ImGui::GetIO().MouseDrawCursor = false;   // 系统光标由引擎显示，不画第二个
        } else {
            // The input hook freezes the real cursor and ImGui draws its own
            // (MouseDrawCursor), so the real one is hidden while the menu is open.
            // Only hide it when it was visible; a hidden (gameplay) cursor is left
            // untouched so closing our menu cannot break the game's cursor state.
            CURSORINFO ci{ sizeof(CURSORINFO) };
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                g_ov.gw->ShowCursor(false);
                g_ov.cursorHiddenByUs = true;
            } else {
                g_ov.cursorHiddenByUs = false;
            }
            g_ov.gw->UnclipCursor();
            StealGameRawMouse();
            // The cursor sits over OUR overlay window, so its visibility follows
            // THIS thread's ShowCursor count - hiding it here never touches the
            // game's own bookkeeping (the game-thread drain wedged the game's
            // input state). ImGui draws the only cursor.
            ImGui::GetIO().MouseDrawCursor = true;
            while (ShowCursor(FALSE) >= 0) {}
        }
        // L3: a new session must seed its cursor from the real GetCursorPos, not
        // from Move events left unread when the previous session closed; L1
        // counters are per session as well.
        InputClearPendingMoves();
        InputResetDropStats();
        g_ov.mousePosValid = false;
        // Seed the edge detector so the first frame after open does not count
        // as a "game regained foreground" transition.
        g_ov.gameWasFg = g_ov.gw->hwnd && GetForegroundWindow() == g_ov.gw->hwnd;
        QueryPerformanceCounter(&g_ov.qpcLast);
        Log("[ov] menu open (hotkey, cursorHiddenByUs=%d)", (int)g_ov.cursorHiddenByUs);
    } else {
        // H2: a pending key capture belongs to the open menu. Clear it on close
        // so (a) the hook stops swallowing gameplay keys and (b) the stale
        // (config,item) target cannot be written after the page is gone.
        g_captureMode.store(0, std::memory_order_relaxed);
        g_captureKind.store(CAP_NONE, std::memory_order_relaxed);
        g_captureIndex.store(-1, std::memory_order_relaxed);
        g_capturedVk.store(0, std::memory_order_release);
        g_capturedMods.store(0, std::memory_order_release);
        g_capCfg = nullptr;
        g_capItem = nullptr;

        ShowWindow(g_ov.hwnd, SW_HIDE);
        ImGui::GetIO().MouseDrawCursor = false;
        if (EnginePortActive()) {
            // 方案 C：由引擎线程解冻并恢复光标，这里不做任何 OS 级恢复动作。
            EnginePortSetMenuOpen(false);
            g_ov.cursorHiddenByUs = false;
        } else {
            RestoreGameRawMouse();
            ShowCursor(TRUE);   // our queue: back to 0 (visible), exact undo of the open drain
        }
        // L2: a key/button held while the menu is toggled off never produces a
        // queue event (the hook passes it to the game once closed), so ImGui
        // would keep it "down" until the next reset. Clear both here so a
        // reopen never starts with phantom held input.
        ImGui::GetIO().ClearInputKeys();
        ImGui::GetIO().ClearInputMouse();
        InputResetDropStats();
        if (!EnginePortActive() && g_ov.cursorHiddenByUs) {
            g_ov.gw->ShowCursor(true);   // force back to the pre-menu visible state
            g_ov.cursorHiddenByUs = false;
            // The posted restore runs on the game window thread; verify the
            // actual global cursor state one second later.
            QueryPerformanceCounter(&g_ov.qpcNow);
            g_ov.cursorVerifyAt.QuadPart = g_ov.qpcNow.QuadPart + g_ov.qpcFreq.QuadPart;
            g_ov.cursorVerifyPending = true;
        }
        g_ov.cfg->Save(g_ov.iniPath);
        SchemaSaveAllDirty();
        Log("[ov] menu closed (config saved)");
    }
    g_menuOpen.store(v, std::memory_order_release);
    RegistryDispatchMenuEvent(v);   // v2 menu open/close listeners (render thread)
    if (v && !EnginePortActive()) InputRehook();   // jump our hooks to the LL chain front (see input.h)
}

void CleanupGraphics() {
    TexturesShutdown();   // SRVs reference the device being torn down below
    if (g_ov.imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_ov.imguiReady = false;
    }
    ResetD3DObjects();
    // M11: release the graphics DLL references exactly once, on the way down.
    auto release = [](HMODULE& h) {
        if (h) {
            FreeLibrary(h);
            h = nullptr;
        }
    };
    release(g_ov.hD3d11);
    release(g_ov.hDxgi);
    release(g_ov.hDcomp);
    release(g_ov.hD3dc);
    if (g_ov.hwnd) {
        DestroyWindow(g_ov.hwnd);
        g_ov.hwnd = nullptr;
    }
}

// H7: called only once the overlay window is destroyed (CleanupGraphics on the
// render thread) and the game WndProc restore has run (OverlayStop). Never while
// a window still uses the class - Windows refuses that, and the class must not
// linger after the DLL unloads.
void UnregisterOverlayWindowClass() {
    if (!g_ov.classRegistered) return;
    if (g_ov.hwnd) {
        Log("[ov] overlay window still alive - window class left registered");
        return;
    }
    if (!UnregisterClassW(g_ov.className.c_str(), g_ov.classInstance)) {
        Log("[ov] WARNING: UnregisterClassW failed err=%lu (class=%ls)", GetLastError(),
            g_ov.className.c_str());
    } else {
        Log("[ov] overlay window class unregistered");
    }
    g_ov.classRegistered = false;
    g_ov.classInstance = nullptr;
    g_ov.className.clear();
}

DWORD WINAPI RenderThread(LPVOID) {
    Log("[ov] render thread start");
    if (!CreateOverlayWindow()) return 1;
    if (!InitD3DAndComposition()) { CleanupGraphics(); return 1; }
    if (!InitPremulPipeline()) { Log("[ov] premul pipeline failed"); CleanupGraphics(); return 1; }
    if (!InitImGui()) { CleanupGraphics(); return 1; }
    QueryPerformanceFrequency(&g_ov.qpcFreq);
    QueryPerformanceCounter(&g_ov.qpcLast);
    Log("[ov] overlay ready - waiting for hotkey");

    bool gameValid = true;
    int findTick = 0;
    while (g_running.load(std::memory_order_relaxed)) {
        // H6: mod hotkey callbacks are queued by the keyboard hook and executed
        // here on the render thread (menu open or closed), never inside the LL
        // hook where a slow callback would stall all game input.
        RegistryFlushQueuedHotkeys();

        if (g_ov.graphicsLost) {
            // M12: degraded mode - the menu cannot render, so swallow toggle
            // requests while gameplay hotkeys (above) and shutdown stay clean.
            g_wantToggle.store(false, std::memory_order_relaxed);
            g_closeRequest.store(false, std::memory_order_relaxed);
            Sleep(100);
            continue;
        }

        if (g_wantToggle.exchange(false, std::memory_order_acq_rel)) SetVisible(!g_ov.visible);
        if (g_closeRequest.exchange(false, std::memory_order_acq_rel)) SetVisible(false);

        if (g_ov.gw->Valid()) {
            if (!gameValid) Log("[ov] game window reacquired");
            gameValid = true;
            if (g_ov.gw->UpdateGeometry() && g_ov.visible) {
                SetWindowPos(g_ov.hwnd, HWND_TOPMOST, g_ov.gw->x, g_ov.gw->y, g_ov.gw->w, g_ov.gw->h, SWP_NOACTIVATE);
                if (g_ov.gw->w != g_ov.w || g_ov.gw->h != g_ov.h) ResizeSwapchain(g_ov.gw->w, g_ov.gw->h);
            }
        } else {
            if (gameValid) {
                SetVisible(false);
                Log("[ov] game window lost; reacquiring");
            }
            gameValid = false;
            if (++findTick >= 60 && g_ov.gw->Find()) findTick = 0;  // ~2 s
        }

        if (g_ov.visible) {
            MSG msg;
            while (PeekMessageW(&msg, g_ov.hwnd, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            RenderFrame();
        } else {
            if (g_ov.cursorVerifyPending) {
                QueryPerformanceCounter(&g_ov.qpcNow);
                if (g_ov.qpcNow.QuadPart >= g_ov.cursorVerifyAt.QuadPart) {
                    g_ov.cursorVerifyPending = false;
                    CURSORINFO ci{ sizeof(CURSORINFO) };
                    bool visible = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0;
                    Log("[ov] cursor verify (1s after close): visible=%d", (int)visible);
                }
            }
            Sleep(30);
        }
    }

    SetVisible(false);
    CleanupGraphics();
    Log("[ov] render thread exit");
    return 0;
}

} // namespace

ID3D11Device* OverlayDevice() {
    return g_ov.dev.Get();
}

bool OverlayStart(GameWindow* gw, Config* cfg, I18n* i18n, const std::wstring& iniPath) {
    g_ov.gw = gw;
    g_ov.cfg = cfg;
    g_ov.i18n = i18n;
    g_ov.iniPath = iniPath;
    g_ov.thread = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);
    if (!g_ov.thread) {
        Log("[ov] render thread creation failed");
        return false;
    }
    // Wait for the window (=> init succeeded) or thread death (=> init failed).
    for (int i = 0; i < 200; ++i) {
        if (g_ov.hwnd) return true;
        DWORD wait = WaitForSingleObject(g_ov.thread, 100);
        if (wait == WAIT_OBJECT_0) {
            CloseHandle(g_ov.thread);
            g_ov.thread = nullptr;
            Log("[ov] overlay init failed");
            return false;
        }
    }
    // M12: a 20 s no-window timeout is an init FAILURE - the old "return true"
    // let the caller believe the overlay was usable while the render thread was
    // still stuck. OverlayStop() by the caller then applies the B2/H9 rollback
    // and pins the module if the thread cannot be joined.
    Log("[ov] ERROR: overlay init timed out after 20s (no window)");
    return false;
}

bool OverlayStop() {
    if (g_ov.thread) {
        if (WaitForSingleObject(g_ov.thread, 5000) != WAIT_OBJECT_0) {
            // B2/H9: do not continue with the restore while the render thread
            // may still be using the game window/overlay state. Keep the handle
            // (and the subclass) and report the failure instead of pretending
            // we unloaded cleanly - the caller pins the module on false.
            Log("[ov] ERROR: render thread still running after 5s - skipping WndProc restore");
            return false;
        }
        CloseHandle(g_ov.thread);
        g_ov.thread = nullptr;
    }
    // B2: the render thread is confirmed gone, so the game window can no
    // longer be re-found/re-subclassed underneath us; undo the subclass now.
    bool ok = true;
    if (g_ov.gw && !g_ov.gw->Detach()) {
        Log("[ov] ERROR: game window subclass restore failed");
        ok = false;
    }
    // H7: the overlay window is already destroyed (CleanupGraphics ran on the
    // render thread before it exited) and the game WndProc restore has been
    // attempted; only now is it safe to drop the class atom. Doing this before
    // the join could unregister a class a live window still uses.
    UnregisterOverlayWindowClass();
    return ok;
}

} // namespace pbzfm
