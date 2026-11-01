#include "game_window.h"
#include "common.h"

namespace pbzfm {

namespace {

constexpr UINT WM_APP_PBZFM_CURSOR = WM_APP + 0x25;
// wParam: 1 = force visible (loop ShowCursor(TRUE) until count >= 0) + unclip,
//         2 = single ShowCursor(FALSE) (restore hidden),
//         3 = ClipCursor(nullptr) only

bool IsCandidate(HWND h) {
    if (!h || !IsWindow(h) || !IsWindowVisible(h)) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return false;
    wchar_t cls[64] = {};
    if (!GetClassNameW(h, cls, 64) || wcscmp(cls, L"UnrealWindow") != 0) return false;
    RECT r{};
    if (!GetClientRect(h, &r)) return false;
    return (r.right - r.left) >= 200 && (r.bottom - r.top) >= 200;
}

WNDPROC g_origProc = nullptr;
HWND    g_subclassedHwnd = nullptr;

LRESULT CALLBACK GameWndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_APP_PBZFM_CURSOR) {
        if (w == 1) {
            while (ShowCursor(TRUE) < 0) {}
            ClipCursor(nullptr);
        } else if (w == 2) {
            ShowCursor(FALSE);  // exactly once: restore the pre-menu counter delta
        } else if (w == 3) {
            ClipCursor(nullptr);
        }
        return 0;
    }
    return CallWindowProc(g_origProc ? g_origProc : DefWindowProcW, h, msg, w, l);
}

} // namespace

bool GameWindow::Find() {
    Detach();
    struct Ctx { HWND best = nullptr; long bestArea = 0; } ctx;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx* c = (Ctx*)lp;
        if (!IsCandidate(h)) return TRUE;
        RECT r{};
        GetClientRect(h, &r);
        long area = (long)(r.right - r.left) * (r.bottom - r.top);
        if (area > c->bestArea) { c->bestArea = area; c->best = h; }
        return TRUE;
    }, (LPARAM)&ctx);
    if (!ctx.best) return false;
    hwnd = ctx.best;
    WNDPROC prev = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)GameWndProc);
    if (!prev && GetLastError() != 0) {
        Log("[gw] subclass failed (err=%lu)", GetLastError());
        hwnd = nullptr;
        return false;
    }
    if (prev != GameWndProc) g_origProc = prev;  // keep original when already subclassed
    g_subclassedHwnd = hwnd;
    UpdateGeometry();
    Log("[gw] attached to game window %p (client %dx%d at %d,%d)", (void*)hwnd, w, h, x, y);
    return true;
}

void GameWindow::Detach() {
    if (g_subclassedHwnd && IsWindow(g_subclassedHwnd) && g_origProc) {
        SetWindowLongPtrW(g_subclassedHwnd, GWLP_WNDPROC, (LONG_PTR)g_origProc);
    }
    g_origProc = nullptr;
    g_subclassedHwnd = nullptr;
    hwnd = nullptr;
}

bool GameWindow::Valid() const {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) return false;
    wchar_t cls[64] = {};
    return GetClassNameW(hwnd, cls, 64) && wcscmp(cls, L"UnrealWindow") == 0;
}

bool GameWindow::UpdateGeometry() {
    if (!Valid()) return false;
    RECT r{};
    if (!GetClientRect(hwnd, &r)) return false;
    POINT origin{ r.left, r.top };
    ClientToScreen(hwnd, &origin);
    int nx = origin.x, ny = origin.y, nw = r.right - r.left, nh = r.bottom - r.top;
    bool changed = (nx != x || ny != y || nw != w || nh != h);
    x = nx; y = ny; w = nw; h = nh;
    return changed;
}

void GameWindow::ShowCursor(bool show) {
    if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_APP_PBZFM_CURSOR, show ? 1 : 2, 0);
}

void GameWindow::UnclipCursor() {
    if (hwnd && IsWindow(hwnd)) PostMessageW(hwnd, WM_APP_PBZFM_CURSOR, 3, 0);
}

} // namespace pbzfm
