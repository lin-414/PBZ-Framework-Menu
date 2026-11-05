#include "game_window.h"
#include "common.h"
#include "engine_port.h"
#include "input.h"

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

    // ---- 方案 C：引擎模式的键盘通道 ----------------------------------------
    // 引擎模式不装全局钩子；但游戏窗口仍是前台窗口（覆盖窗是 WS_EX_NOACTIVATE），
    // 所以按键消息一定经过这里。菜单开着时吞掉并喂给 ImGui，关着时只认呼出键，
    // 其余原样放行 —— 系统热键（录屏/截图/游戏栏）因此不受任何影响。
    if (EnginePortActive()) {
        switch (msg) {
            // 游戏重新激活/拿到焦点：本作会重建输入栈（实机：N 卡浮层抢焦点回来后，
            // 左键会漏给 PBZPhotoMode）。这里立刻请求引擎线程补一次冻结；菜单没开时是空操作。
            // 必须原样放行给游戏，否则它自己的激活逻辑会丢。
            case WM_ACTIVATE:
            case WM_SETFOCUS:
                if (msg == WM_SETFOCUS || LOWORD(w) != WA_INACTIVE) EnginePortReassert();
                return CallWindowProc(g_origProc ? g_origProc : DefWindowProcW, h, msg, w, l);
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            case WM_KEYUP:
            case WM_SYSKEYUP: {
                const unsigned vk = (unsigned)w;
                const bool down = (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
                const bool menuOpen = g_menuOpen.load(std::memory_order_relaxed);

                if (vk == g_toggleVk.load(std::memory_order_relaxed)) {
                    if (down) {
                        if (menuOpen) g_closeRequest.store(true, std::memory_order_release);
                        else g_wantToggle.store(true, std::memory_order_release);
                    }
                    return 0;
                }
                if (!menuOpen) return CallWindowProc(g_origProc ? g_origProc : DefWindowProcW, h, msg, w, l);

                // 键位捕获（设置页点组合键后等一次真实按键）：钩子路径在 KbProc 里做，
                // 这里等价实现，捕获态优先于喂菜单。
                if (g_captureMode.load(std::memory_order_relaxed) == 1) {
                    const bool modifier = (vk >= VK_SHIFT && vk <= VK_MENU) || vk == VK_LWIN || vk == VK_RWIN;
                    if (down && !modifier) {
                        unsigned mods = 0;
                        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= 0x1u;
                        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) mods |= 0x2u;
                        if (GetAsyncKeyState(VK_MENU) & 0x8000) mods |= 0x4u;
                        g_capturedMods.store(mods, std::memory_order_release);
                        g_capturedVk.store(vk, std::memory_order_release);
                        g_captureMode.store(0, std::memory_order_relaxed);
                    }
                    return 0;
                }

                InputPushKeyVk(vk, down);
                return 0;
            }
            case WM_CHAR:
                return 0;   // 菜单自己处理文本输入，别让字符消息漏给游戏
            default:
                break;
        }
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
