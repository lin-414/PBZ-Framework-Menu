// DirectComposition overlay: own top-level window + own D3D11 device +
// swapchain created with CreateSwapChainForComposition. Nothing here touches
// the game's swap chain or D3D objects, so FSR3 frame generation / Streamline
// (which killed the Present-hook route, see PBZPhotoMode PROGRESS 1.1.x) is
// unaffected. Graphics DLLs are loaded at runtime, after the game window
// exists, so the PE import table stays clean while UE4SS loads us early.
#include "overlay.h"

#include "common.h"
#include "config.h"
#include "i18n.h"
#include "game_window.h"
#include "input.h"
#include "menu.h"
#include "layout.h"
#include "schema.h"
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

constexpr wchar_t kOverlayClass[] = L"PBZFMOverlayWnd";

struct Overlay {
    HWND hwnd = nullptr;
    bool visible = false;
    int  w = 0, h = 0;

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

    bool imguiReady = false;
    bool cursorHiddenByUs = false;  // we hid a previously visible cursor, restore on close
    bool mousePosValid = false;     // ImGui mouse position known since menu open
    bool cursorVerifyPending = false;   // log the actual cursor state ~1s after restore
    LARGE_INTEGER cursorVerifyAt{};
    LARGE_INTEGER qpcNow{};
    LARGE_INTEGER qpcFreq{};
    LARGE_INTEGER qpcLast{};
    LARGE_INTEGER diagNext{};           // 1 Hz input/present diagnostics while the menu is open
    float presentMaxMs = 0.0f;
    std::vector<RAWINPUTDEVICE> rawList;   // game's raw-input registrations, saved on open
    bool rawListSaved = false;

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
bool StealGameRawMouse() {
    UINT n = 0;
    GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
    std::vector<RAWINPUTDEVICE> saved(n);
    UINT got = n;
    if (n > 0 && GetRegisteredRawInputDevices(saved.data(), &got, sizeof(RAWINPUTDEVICE)) == 0) return false;
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
        Log("[ov] raw mouse re-point failed err=%lu", GetLastError());
        return false;
    }
    g_ov.rawList = saved;
    g_ov.rawListSaved = true;
    Log("[ov] raw mouse re-pointed to overlay (%u entries saved)", (unsigned)got);
    return true;
}

void RestoreGameRawMouse() {
    // A saved list can be empty (nothing was registered before us): with no
    // game entry to restore, fall through to removing ours - registering an
    // empty list fails with ERROR_INVALID_PARAMETER and would leave our
    // overlay registration live while reporting a restore.
    if (g_ov.rawListSaved && !g_ov.rawList.empty()) {
        if (!RegisterRawInputDevices(g_ov.rawList.data(), (UINT)g_ov.rawList.size(), sizeof(RAWINPUTDEVICE)))
            Log("[ov] WARNING: raw input restore failed err=%lu", GetLastError());
        else
            Log("[ov] raw input registrations restored to game");
        g_ov.rawListSaved = false;
    } else {
        // We registered without a game entry existing: remove only ours.
        RAWINPUTDEVICE rm{};
        rm.usUsagePage = 0x01;
        rm.usUsage = 0x02;
        rm.dwFlags = RIDEV_REMOVE;   // safe: no game mouse entry exists to destroy
        rm.hwndTarget = nullptr;
        if (!RegisterRawInputDevices(&rm, 1, sizeof(rm)))
            Log("[ov] raw mouse removal failed err=%lu", GetLastError());
    }
}

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
            auto* px = (unsigned char*)bits;
            for (int i = 0; i < gw->w * gw->h; ++i) std::swap(px[i * 4 + 0], px[i * 4 + 2]);  // BGRA -> RGBA
            png.reserve((size_t)gw->w * gw->h * 4);
            stbi_write_png_to_func([](void* ctx, void* data, int size) {
                auto* out = (std::vector<unsigned char>*)ctx;
                const auto* p = (const unsigned char*)data;
                out->insert(out->end(), p, p + size);
            }, &png, gw->w, gw->h, 4, px, gw->w * 4);
            ok = !png.empty();
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

bool CreateOverlayWindow() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = OverlayWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kOverlayClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Log("[ov] RegisterClassEx failed err=%lu", GetLastError());
        return false;
    }
    g_ov.w = g_ov.gw->w;
    g_ov.h = g_ov.gw->h;
    g_ov.hwnd = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kOverlayClass, L"PBZFM Overlay", WS_POPUP,
        g_ov.gw->x, g_ov.gw->y, g_ov.w, g_ov.h,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_ov.hwnd) {
        Log("[ov] CreateWindowEx failed err=%lu", GetLastError());
        return false;
    }
    Log("[ov] overlay window created (%dx%d at %d,%d)", g_ov.w, g_ov.h, g_ov.gw->x, g_ov.gw->y);
    return true;
}

bool LoadGraphicsApis(PFN_D3D11CreateDevice* d3dCreate, PFN_CreateDXGIFactory2* factory2,
                      PFN_DCompositionCreateDevice* dcompCreate, PFN_D3DCompile* d3dCompile) {
    HMODULE hd3d11 = LoadLibraryW(L"d3d11.dll");
    HMODULE hdxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE hdcomp = LoadLibraryW(L"dcomp.dll");
    HMODULE hd3dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!hd3d11 || !hdxgi || !hdcomp || !hd3dc) {
        Log("[ov] graphics dll load failed: d3d11=%p dxgi=%p dcomp=%p d3dcompiler=%p",
            (void*)hd3d11, (void*)hdxgi, (void*)hdcomp, (void*)hd3dc);
        return false;
    }
    *d3dCreate = (PFN_D3D11CreateDevice)GetProcAddress(hd3d11, "D3D11CreateDevice");
    *factory2 = (PFN_CreateDXGIFactory2)GetProcAddress(hdxgi, "CreateDXGIFactory2");
    *dcompCreate = (PFN_DCompositionCreateDevice)GetProcAddress(hdcomp, "DCompositionCreateDevice");
    *d3dCompile = (PFN_D3DCompile)GetProcAddress(hd3dc, "D3DCompile");
    if (!*d3dCreate || !*factory2 || !*dcompCreate || !*d3dCompile) {
        Log("[ov] graphics proc resolve failed");
        return false;
    }
    return true;
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
    HMODULE hd3dc = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!hd3dc) return false;
    PFN_D3DCompile d3dCompile = (PFN_D3DCompile)GetProcAddress(hd3dc, "D3DCompile");
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

    bool fontOk = false;
    // Malgun Gothic is the only candidate with Hangul, but its hanja coverage
    // is too poor to serve Chinese, so it leads the list only for a Korean UI;
    // everything else leads with YaHei (kana + CJK ideographs covered).
    static const char* const kFontsDefault[] = {
        "C:/Windows/Fonts/msyh.ttc",      // Microsoft YaHei (simplified Chinese UI font)
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/malgun.ttf",    // Malgun Gothic (Korean)
        "C:/Windows/Fonts/malgunbd.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
    };
    static const char* const kFontsKorean[] = {
        "C:/Windows/Fonts/malgun.ttf",
        "C:/Windows/Fonts/malgunbd.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/segoeui.ttf",
    };
    const bool koreanUi = g_ov.cfg && g_ov.cfg->language == "ko-KR";
    const char* const* fonts = koreanUi ? kFontsKorean : kFontsDefault;
    const int fontCount = koreanUi ? 4 : 5;
    for (int i = 0; i < fontCount; ++i) {
        if (io.Fonts->AddFontFromFileTTF(fonts[i], 18.0f)) {
            fontOk = true;
            Log("[ov] font loaded: %s", fonts[i]);
            break;
        }
    }
    if (!fontOk) {
        io.Fonts->AddFontDefault();
        Log("[ov] WARNING: no system font found, falling back to ImGui default (CJK will show boxes)");
    }

    if (!ImGui_ImplDX11_Init(g_ov.dev.Get(), g_ov.ctx.Get())) {
        Log("[ov] ImGui_ImplDX11_Init failed");
        return false;
    }

    theme::ApplyPbzTheme(g_ov.cfg ? g_ov.cfg->uiOpacity : 0.94f);

    g_ov.imguiReady = true;
    Log("[ov] ImGui ready (backend dx11, dynamic CJK fonts)");
    return true;
}

void RenderFrame() {
    if (!EnsureRtv()) return;  // retry next frame

    ImGuiIO& io = ImGui::GetIO();

    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - g_ov.qpcLast.QuadPart) / (double)g_ov.qpcFreq.QuadPart);
    g_ov.qpcLast = now;
    dt = std::clamp(dt, 1.0f / 240.0f, 0.1f);

    InputEvent ev;
    bool gotPos = false;
    while (InputPop(&ev)) {
        switch (ev.type) {
            case InputEvent::Key: io.AddKeyEvent((ImGuiKey)ev.key, ev.down); break;
            case InputEvent::Btn: io.AddMouseButtonEvent(ev.key, ev.down); break;
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
        } else {
            g_ov.cfg->toggleVk = captured;
            g_toggleVk.store(captured, std::memory_order_relaxed);
            g_ov.cfg->Save(g_ov.iniPath);
            Log("[ov] toggle key captured: %s (config saved)", Config::VkName(captured));
        }
        g_captureKind.store(CAP_NONE, std::memory_order_relaxed);
    }

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
    g_ov.sc->Present(1, 0);
    g_ov.ddev->Commit();
    QueryPerformanceCounter(&p1);
    float pms = (float)((double)(p1.QuadPart - p0.QuadPart) * 1000.0 / (double)g_ov.qpcFreq.QuadPart);
    if (pms > g_ov.presentMaxMs) g_ov.presentMaxMs = pms;
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
        g_ov.mousePosValid = false;
        ImGui::GetIO().MouseDrawCursor = true;
        // The cursor sits over OUR overlay window, so its visibility follows
        // THIS thread's ShowCursor count - hiding it here never touches the
        // game's own bookkeeping (the game-thread drain wedged the game's
        // input state). ImGui draws the only cursor.
        while (ShowCursor(FALSE) >= 0) {}
        QueryPerformanceCounter(&g_ov.qpcLast);
        Log("[ov] menu open (hotkey, cursorHiddenByUs=%d)", (int)g_ov.cursorHiddenByUs);
    } else {
        ShowWindow(g_ov.hwnd, SW_HIDE);
        RestoreGameRawMouse();
        ImGui::GetIO().MouseDrawCursor = false;
        ShowCursor(TRUE);   // our queue: back to 0 (visible), exact undo of the open drain
        if (g_ov.cursorHiddenByUs) {
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
    if (v) InputRehook();   // jump our hooks to the LL chain front (see input.h)
}

void CleanupGraphics() {
    if (g_ov.imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
        g_ov.imguiReady = false;
    }
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
    if (g_ov.hwnd) {
        DestroyWindow(g_ov.hwnd);
        g_ov.hwnd = nullptr;
    }
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
    return true;
}

void OverlayStop() {
    if (g_ov.thread) {
        if (WaitForSingleObject(g_ov.thread, 5000) != WAIT_OBJECT_0) {
            Log("[ov] WARNING: render thread did not exit within 5s");
        }
        CloseHandle(g_ov.thread);
        g_ov.thread = nullptr;
    }
}

} // namespace pbzfm
