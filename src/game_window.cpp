#include "game_window.h"
#include "common.h"

namespace pbzfm {

namespace {

constexpr UINT WM_APP_PBZFM_CURSOR = WM_APP + 0x25;
// wParam: 1 = force visible (loop ShowCursor(TRUE) until count >= 0) + unclip,
//         2 = single ShowCursor(FALSE) (restore hidden),
//         3 = ClipCursor(nullptr) only

// B2: posted by Detach() when the caller is not the game window's thread; the
// handler runs on that thread and undoes the subclass there.
constexpr UINT WM_APP_PBZFM_DETACH = WM_APP + 0x28;

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
    if (msg == WM_APP_PBZFM_DETACH) {
        // B2: runs on the game window's thread, the only thread on which
        // SetWindowLongPtrW is guaranteed to be in effect for the next
        // dispatch. Clear the globals first so a duplicated request is a
        // no-op (Detach keeps its state on timeout and may retry).
        HWND target = g_subclassedHwnd ? g_subclassedHwnd : h;
        WNDPROC orig = g_origProc;
        g_origProc = nullptr;
        g_subclassedHwnd = nullptr;
        if (orig && IsWindow(target)) SetWindowLongPtrW(target, GWLP_WNDPROC, (LONG_PTR)orig);
        return 0;
    }
    return CallWindowProc(g_origProc ? g_origProc : DefWindowProcW, h, msg, w, l);
}

} // namespace

bool GameWindow::Find() {
    // B2: never attach a second subclass while the previous restore is
    // unresolved - the original WndProc would be lost for good.
    if (!Detach()) {
        Log("[gw] refusing to attach: previous WndProc restore failed");
        return false;
    }
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
    g_gameHwnd.store((void*)hwnd, std::memory_order_relaxed);
    UpdateGeometry();
    Log("[gw] attached to game window %p (client %dx%d at %d,%d)", (void*)hwnd, w, h, x, y);
    return true;
}

bool GameWindow::Detach() {
    HWND h = g_subclassedHwnd;
    if (!h || !IsWindow(h) || !g_origProc) {
        // Never attached, or the window is already gone: nothing left to
        // restore, so clear the tracking state and report success.
        g_origProc = nullptr;
        g_subclassedHwnd = nullptr;
        hwnd = nullptr;
        g_gameHwnd.store(nullptr, std::memory_order_relaxed);
        return true;
    }
    if (GetWindowThreadProcessId(h, nullptr) == GetCurrentThreadId()) {
        WNDPROC orig = g_origProc;
        g_origProc = nullptr;
        g_subclassedHwnd = nullptr;
        SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)orig);
    } else {
        // B2: SetWindowLongPtrW from a foreign thread is not guaranteed to be
        // visible to the window dispatch before the next message. Post the
        // restore to the game window's thread and wait for it, so callers can
        // tell "restored" apart from "still subclassed".
        DWORD_PTR result = 0;
        LRESULT sent = SendMessageTimeoutW(h, WM_APP_PBZFM_DETACH, 0, 0,
                                           SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
        if (!sent) {
            Log("[gw] ERROR: subclass restore failed (hwnd=%p err=%lu)", (void*)h, GetLastError());
            return false;   // keep the state: a retry can still restore it
        }
        if (g_origProc) {
            Log("[gw] ERROR: subclass restore not applied (hwnd=%p)", (void*)h);
            return false;
        }
    }
    hwnd = nullptr;
    g_gameHwnd.store(nullptr, std::memory_order_relaxed);
    Log("[gw] subclass restored");
    return true;
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
