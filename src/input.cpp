#include "input.h"
#include "common.h"
#include "registry.h"
#include <imgui.h>
#include <mutex>
#include <deque>

namespace pbzfm {

namespace {

std::mutex g_queueMutex;
std::deque<InputEvent> g_queue;

void Push(const InputEvent& e) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.size() < 256) g_queue.push_back(e);
}

// Motion carries position only, so a run of moves collapses into the newest
// one: the queue stays bounded on high-polling mice and the hook stays fast.
void PushMove(float x, float y) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (!g_queue.empty() && g_queue.back().type == InputEvent::Move) {
        g_queue.back().wx = x;
        g_queue.back().wy = y;
        return;
    }
    if (g_queue.size() < 256) {
        InputEvent e{};
        e.type = InputEvent::Move;
        e.wx = x;
        e.wy = y;
        g_queue.push_back(e);
    }
}

HANDLE g_hookThread = nullptr;
DWORD  g_hookThreadId = 0;
HHOOK g_kbHook = nullptr;
HHOOK g_mouseHook = nullptr;
bool g_toggleWasDown = false;

// Thread message: unhook + rehook, making our hooks the newest (first run)
// in the LL chain again. Posted from the render thread on menu open.
constexpr UINT WM_APP_PBZFM_REHOOK = WM_APP + 0x26;

LRESULT CALLBACK KbProc(int code, WPARAM w, LPARAM l);
LRESULT CALLBACK MouseProc(int code, WPARAM w, LPARAM l);

// Menu-open motion tracking (hook thread only). Every move is eaten, so the
// OS cursor never advances and each event's pt is (frozen position + this
// event's delta, clamped by whatever ClipCursor the game re-asserted).
// Accumulate the per-event delta (pt - current real position) into the
// would-be cursor position for ImGui. A SetCursorPos teleport self-zeroes
// (its pt equals the real position) and a real-cursor jump behind our back
// is absorbed by the rebase instead of being followed.
struct MoveTrack {
    bool valid = false;
    LONG baseX = 0, baseY = 0;    // real cursor position the last event saw
    LONG trackX = 0, trackY = 0;  // would-be cursor position for ImGui
};
MoveTrack g_moveTrack;

void RehookNow() {
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log("[input] hooks re-queued to chain front (kb=%p mouse=%p)", (void*)g_kbHook, (void*)g_mouseHook);
}

int VkToImGuiKey(UINT vk) {
    switch (vk) {
        case '0': return ImGuiKey_0; case '1': return ImGuiKey_1; case '2': return ImGuiKey_2;
        case '3': return ImGuiKey_3; case '4': return ImGuiKey_4; case '5': return ImGuiKey_5;
        case '6': return ImGuiKey_6; case '7': return ImGuiKey_7; case '8': return ImGuiKey_8;
        case '9': return ImGuiKey_9;
        case 'A': return ImGuiKey_A; case 'B': return ImGuiKey_B; case 'C': return ImGuiKey_C;
        case 'D': return ImGuiKey_D; case 'E': return ImGuiKey_E; case 'F': return ImGuiKey_F;
        case 'G': return ImGuiKey_G; case 'H': return ImGuiKey_H; case 'I': return ImGuiKey_I;
        case 'J': return ImGuiKey_J; case 'K': return ImGuiKey_K; case 'L': return ImGuiKey_L;
        case 'M': return ImGuiKey_M; case 'N': return ImGuiKey_N; case 'O': return ImGuiKey_O;
        case 'P': return ImGuiKey_P; case 'Q': return ImGuiKey_Q; case 'R': return ImGuiKey_R;
        case 'S': return ImGuiKey_S; case 'T': return ImGuiKey_T; case 'U': return ImGuiKey_U;
        case 'V': return ImGuiKey_V; case 'W': return ImGuiKey_W; case 'X': return ImGuiKey_X;
        case 'Y': return ImGuiKey_Y; case 'Z': return ImGuiKey_Z;
        case VK_NUMPAD0: return ImGuiKey_Keypad0; case VK_NUMPAD1: return ImGuiKey_Keypad1;
        case VK_NUMPAD2: return ImGuiKey_Keypad2; case VK_NUMPAD3: return ImGuiKey_Keypad3;
        case VK_NUMPAD4: return ImGuiKey_Keypad4; case VK_NUMPAD5: return ImGuiKey_Keypad5;
        case VK_NUMPAD6: return ImGuiKey_Keypad6; case VK_NUMPAD7: return ImGuiKey_Keypad7;
        case VK_NUMPAD8: return ImGuiKey_Keypad8; case VK_NUMPAD9: return ImGuiKey_Keypad9;
        case VK_MULTIPLY: return ImGuiKey_KeypadMultiply; case VK_ADD: return ImGuiKey_KeypadAdd;
        case VK_SUBTRACT: return ImGuiKey_KeypadSubtract; case VK_DECIMAL: return ImGuiKey_KeypadDecimal;
        case VK_DIVIDE: return ImGuiKey_KeypadDivide;
        case VK_F1: return ImGuiKey_F1; case VK_F2: return ImGuiKey_F2; case VK_F3: return ImGuiKey_F3;
        case VK_F4: return ImGuiKey_F4; case VK_F5: return ImGuiKey_F5; case VK_F6: return ImGuiKey_F6;
        case VK_F7: return ImGuiKey_F7; case VK_F8: return ImGuiKey_F8; case VK_F9: return ImGuiKey_F9;
        case VK_F10: return ImGuiKey_F10; case VK_F11: return ImGuiKey_F11; case VK_F12: return ImGuiKey_F12;
        case VK_LEFT: return ImGuiKey_LeftArrow; case VK_RIGHT: return ImGuiKey_RightArrow;
        case VK_UP: return ImGuiKey_UpArrow; case VK_DOWN: return ImGuiKey_DownArrow;
        case VK_SPACE: return ImGuiKey_Space; case VK_TAB: return ImGuiKey_Tab;
        case VK_RETURN: return ImGuiKey_Enter; case VK_BACK: return ImGuiKey_Backspace;
        case VK_ESCAPE: return ImGuiKey_Escape; case VK_INSERT: return ImGuiKey_Insert;
        case VK_DELETE: return ImGuiKey_Delete; case VK_HOME: return ImGuiKey_Home;
        case VK_END: return ImGuiKey_End; case VK_PRIOR: return ImGuiKey_PageUp;
        case VK_NEXT: return ImGuiKey_PageDown;
        case VK_OEM_PLUS: return ImGuiKey_Equal; case VK_OEM_MINUS: return ImGuiKey_Minus;
        case VK_OEM_PERIOD: return ImGuiKey_Period; case VK_OEM_COMMA: return ImGuiKey_Comma;
        case VK_OEM_1: return ImGuiKey_Semicolon; case VK_OEM_2: return ImGuiKey_Slash;
        case VK_OEM_3: return ImGuiKey_GraveAccent; case VK_OEM_4: return ImGuiKey_LeftBracket;
        case VK_OEM_5: return ImGuiKey_Backslash; case VK_OEM_6: return ImGuiKey_RightBracket;
        case VK_OEM_7: return ImGuiKey_Apostrophe;
        // Modifiers are submitted as ImGuiMod_* key events (same as imgui_impl_win32).
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: return (int)ImGuiMod_Shift;
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: return (int)ImGuiMod_Ctrl;
        case VK_MENU: case VK_LMENU: case VK_RMENU: return (int)ImGuiMod_Alt;
        default: return ImGuiKey_None;
    }
}

bool IsModifier(UINT vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_LWIN || vk == VK_RWIN;
}

LRESULT CALLBACK KbProc(int code, WPARAM w, LPARAM l) {
    if (code < 0 || !g_running) return CallNextHookEx(g_kbHook, code, w, l);
    KBDLLHOOKSTRUCT* s = (KBDLLHOOKSTRUCT*)l;
    UINT vk = s->vkCode;
    bool up = (s->flags & LLKHF_UP) != 0;

    if (vk == VK_LWIN || vk == VK_RWIN) return CallNextHookEx(g_kbHook, code, w, l);

    // Key capture flow: first non-modifier key becomes the new toggle key.
    if (g_captureMode.load(std::memory_order_relaxed) == 1) {
        if (!up && !IsModifier(vk)) {
            g_capturedVk.store(vk, std::memory_order_release);
            g_captureMode.store(0, std::memory_order_relaxed);
        }
        return 1;  // never let capture strokes reach the game
    }

    if (vk == g_toggleVk.load(std::memory_order_relaxed)) {
        if (!up && !g_toggleWasDown) g_wantToggle.store(true, std::memory_order_release);
        g_toggleWasDown = !up;
        if (g_menuOpen.load(std::memory_order_relaxed)) return 1;
        return CallNextHookEx(g_kbHook, code, w, l);  // closed: game also sees the key
    }

    if (!g_menuOpen.load(std::memory_order_relaxed)) {
        // Registered mod hotkeys fire while the menu is closed; when one
        // matches, the key is swallowed before the game sees it.
        if (RegistryDispatchHotkey(vk, up)) return 1;
        return CallNextHookEx(g_kbHook, code, w, l);
    }

    // Menu open: swallow everything except Alt+Tab so the user can escape.
    if (vk == VK_TAB && (GetAsyncKeyState(VK_MENU) & 0x8000))
        return CallNextHookEx(g_kbHook, code, w, l);

    // Registered mod hotkeys stay live while the menu is open (screenshot /
    // photo-mode toggles are meta-features, not game input). A dispatched key
    // is swallowed and does not reach the menu widgets either.
    if (RegistryDispatchHotkey(vk, up)) return 1;

    int ik = VkToImGuiKey(vk);
    if (ik != ImGuiKey_None) {
        InputEvent e{};
        e.type = InputEvent::Key;
        e.key = ik;
        e.down = !up;
        Push(e);
    }
    return 1;
}

LRESULT CALLBACK MouseProc(int code, WPARAM w, LPARAM l) {
    if (code < 0 || !g_running) return CallNextHookEx(g_mouseHook, code, w, l);
    if (!g_menuOpen.load(std::memory_order_relaxed)) {
        g_moveTrack.valid = false;   // rebase on the next open
        return CallNextHookEx(g_mouseHook, code, w, l);
    }
    MSLLHOOKSTRUCT* s = (MSLLHOOKSTRUCT*)l;
    InputEvent e{};
    switch (w) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            e.type = InputEvent::Btn; e.key = 0; e.down = (w == WM_LBUTTONDOWN); Push(e); return 1;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            e.type = InputEvent::Btn; e.key = 1; e.down = (w == WM_RBUTTONDOWN); Push(e); return 1;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            e.type = InputEvent::Btn; e.key = 2; e.down = (w == WM_MBUTTONDOWN); Push(e); return 1;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            e.type = InputEvent::Btn; e.key = 3 + HIWORD(s->mouseData); e.down = (w == WM_XBUTTONDOWN); Push(e); return 1;
        case WM_MOUSEWHEEL:
            e.type = InputEvent::Wheel; e.wx = 0.0f; e.wy = (float)GET_WHEEL_DELTA_WPARAM(s->mouseData) / (float)WHEEL_DELTA;
            Push(e); return 1;
        case WM_MOUSEHWHEEL:
            e.type = InputEvent::Wheel; e.wx = (float)GET_WHEEL_DELTA_WPARAM(s->mouseData) / (float)WHEEL_DELTA; e.wy = 0.0f;
            Push(e); return 1;
        case WM_MOUSEMOVE:
            // Eat moves too: a running game reacts to motion via its own hook
            // data or raw input, neither of which cares which window is under
            // the cursor. Injected (SendInput) moves are synthetic - eat them
            // but never treat them as user motion.
            if (!(s->flags & LLMHF_INJECTED)) {
                POINT cur{};
                GetCursorPos(&cur);
                if (!g_moveTrack.valid) {
                    g_moveTrack.baseX = g_moveTrack.trackX = cur.x;
                    g_moveTrack.baseY = g_moveTrack.trackY = cur.y;
                    g_moveTrack.valid = true;
                } else if (cur.x != g_moveTrack.baseX || cur.y != g_moveTrack.baseY) {
                    g_moveTrack.baseX = cur.x;   // the game teleported the real
                    g_moveTrack.baseY = cur.y;   // cursor: absorb, don't follow
                }
                g_moveTrack.trackX += s->pt.x - g_moveTrack.baseX;
                g_moveTrack.trackY += s->pt.y - g_moveTrack.baseY;
                PushMove((float)g_moveTrack.trackX, (float)g_moveTrack.trackY);
                g_hookMoves.fetch_add(1, std::memory_order_relaxed);
            }
            return 1;
        default:
            return 1;
    }
}

DWORD WINAPI HookThread(LPVOID) {
    g_hookThreadId = GetCurrentThreadId();
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
    g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
    Log("[input] hooks installed (kb=%p mouse=%p)", (void*)g_kbHook, (void*)g_mouseHook);
    MSG msg;
    while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP_PBZFM_REHOOK) { RehookNow(); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
    if (g_mouseHook) UnhookWindowsHookEx(g_mouseHook);
    g_kbHook = g_mouseHook = nullptr;
    Log("[input] hooks removed");
    return 0;
}

} // namespace

bool InputStart() {
    g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    return g_hookThread != nullptr;
}

void InputStop() {
    if (g_hookThread) {
        // PostMessage cannot reach GetMessageW reliably without a window; post a quit via thread message.
        PostThreadMessageW(GetThreadId(g_hookThread), WM_QUIT, 0, 0);
        WaitForSingleObject(g_hookThread, 3000);
        CloseHandle(g_hookThread);
        g_hookThread = nullptr;
    }
}

bool InputPop(InputEvent* out) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.empty()) return false;
    *out = g_queue.front();
    g_queue.pop_front();
    return true;
}

void InputPushEvent(const InputEvent& e) { Push(e); }



void InputRehook() {
    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_APP_PBZFM_REHOOK, 0, 0);
}

} // namespace pbzfm
