#include "input.h"
#include "common.h"
#include "registry.h"
#include <imgui.h>
#include <algorithm>
#include <mutex>
#include <set>
#include <deque>

namespace pbzfm {

namespace {

std::mutex g_queueMutex;
std::deque<InputEvent> g_queue;

// L1: the old "queue full -> silently drop" could swallow a key-up and leave
// ImGui convinced the key is still held. Drops are now counted and reported by
// the render thread (throttled), so the hook callback never blocks on logging.
constexpr size_t kQueueMax = 256;
std::atomic<unsigned> g_queueDropPending{ 0 };   // dropped since the last report
std::atomic<unsigned> g_queueDropTotal{ 0 };     // dropped since the last reset
std::atomic<DWORD> g_queueDropLogTick{ 0 };

void NoteDropLocked(unsigned n) {
    g_queueDropPending.fetch_add(n, std::memory_order_relaxed);
    g_queueDropTotal.fetch_add(n, std::memory_order_relaxed);
}

// Lock held: evict the least harmful queued event to make room. Move (position
// only) first, then wheel, then a down-edge. Up-edges are never evicted: losing
// one leaves a key/button logically pressed in ImGui.
bool EvictForRoomLocked() {
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if (it->type == InputEvent::Move) { g_queue.erase(it); return true; }
    }
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if (it->type == InputEvent::Wheel) { g_queue.erase(it); return true; }
    }
    for (auto it = g_queue.begin(); it != g_queue.end(); ++it) {
        if ((it->type == InputEvent::Key || it->type == InputEvent::Btn) && it->down) {
            g_queue.erase(it);
            return true;
        }
    }
    return false;
}

void Push(const InputEvent& e) {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    if (g_queue.size() >= kQueueMax) {
        if (!EvictForRoomLocked()) {
            bool upEdge = (e.type == InputEvent::Key || e.type == InputEvent::Btn) && !e.down;
            // L1: an all-up-edge queue cannot happen from a balanced stream, but
            // if it does keep the newest up-edge (the current state) and drop the
            // oldest one; otherwise keep the old transitions and drop this event.
            if (upEdge) {
                g_queue.pop_front();
            } else {
                NoteDropLocked(1);
                return;
            }
        }
    }
    g_queue.push_back(e);
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
    if (g_queue.size() >= kQueueMax && !EvictForRoomLocked()) {
        NoteDropLocked(1);   // L1: the event that mattered least is another move
        return;
    }
    InputEvent e{};
    e.type = InputEvent::Move;
    e.wx = x;
    e.wy = y;
    g_queue.push_back(e);
}

HANDLE g_hookThread = nullptr;
std::atomic<DWORD> g_hookThreadId{ 0 };
HHOOK g_kbHook = nullptr;
HHOOK g_mouseHook = nullptr;
bool g_toggleWasDown = false;
// H9: all hook handle writes (install, rehook, unhook-on-stop, unhook-on-exit)
// take this mutex, so InputStop() unhooking from the shutting-down thread can
// never race the hook thread's RehookNow() into leaving a live hook behind.
std::mutex g_hookMtx;

void UninstallHooksLocked() {
    if (g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
    if (g_mouseHook) {
        UnhookWindowsHookEx(g_mouseHook);
        g_mouseHook = nullptr;
    }
}

// Thread message: unhook + rehook, making our hooks the newest (first run)
// in the LL chain again. Posted from the render thread on menu open.
constexpr UINT WM_APP_PBZFM_REHOOK = WM_APP + 0x26;
// Thread message: send the pending synthetic chord. Posted from KbProc -
// SendInput from inside a hook callback would re-enter the hook chain.
constexpr UINT WM_APP_PBZFM_REPLAY = WM_APP + 0x27;

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

// ---- screenshot-tool passthrough (see input.h) ----
// Hook-thread state. g_ptChords/g_ptReplay are published via
// InputSetPassthrough (currently once, before the hooks start; the SRWLOCK
// keeps a future live reconfigure race-free). The rest lives only on the
// hook thread - both hooks run there.
constexpr DWORD kPtGraceMs = 1000;   // instant captures never switch foreground
constexpr DWORD kPtFgPumpMs = 15;    // foreground polling rate; a poll per event is waste

SRWLOCK g_ptLock = SRWLOCK_INIT;
std::vector<HotkeyChord> g_ptChords;
bool g_ptReplay = false;
std::atomic<bool> g_ptActive{ false };
DWORD g_ptStartTick = 0;
DWORD g_ptFgPumpTick = 0;
bool g_ptSawOtherFg = false;
bool g_ptOnGame = true;    // pumped foreground snapshot: game window owns the foreground
HotkeyChord g_ptPendingReplay{};
std::set<unsigned> g_ptDownKeys;   // keys passed DOWN during the borrow window

int PtAsync(UINT vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0 ? 1 : 0; }

// Modifier state as seen by this hook. GetAsyncKeyState does not reflect keys
// the LL chain swallowed (verified 2026-11-01: Ctrl+Alt swallowed right before
// the final key, async state still reports all-up) - and the menu-open swallow
// eats exactly the modifiers a chord needs, so the match runs on our own
// bookkeeping instead.
unsigned g_ptMods = 0;   // bit0 ctrl, bit1 shift, bit2 alt, bit3 win (hook thread only)

void PtTrackMods(UINT vk, bool down) {
    unsigned bit;
    switch (vk) {
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL: bit = 1u << 0; break;
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: bit = 1u << 1; break;
        case VK_MENU: case VK_LMENU: case VK_RMENU: bit = 1u << 2; break;
        case VK_LWIN: case VK_RWIN: bit = 1u << 3; break;
        default: return;
    }
    if (down) g_ptMods |= bit; else g_ptMods &= ~bit;
}

unsigned PtAsyncMods() {
    unsigned m = 0;
    if (PtAsync(VK_CONTROL) || PtAsync(VK_LCONTROL) || PtAsync(VK_RCONTROL)) m |= 1u;
    if (PtAsync(VK_SHIFT) || PtAsync(VK_LSHIFT) || PtAsync(VK_RSHIFT)) m |= 2u;
    if (PtAsync(VK_MENU) || PtAsync(VK_LMENU) || PtAsync(VK_RMENU)) m |= 4u;
    if (PtAsync(VK_LWIN) || PtAsync(VK_RWIN)) m |= 8u;
    return m;
}

// Strict modifier match: a held extra Shift does not trigger an Alt+P chord.
bool PtModsMatch(const HotkeyChord& c) {
    bool ctrl = g_ptMods & 1u, shift = g_ptMods & 2u, alt = g_ptMods & 4u, win = g_ptMods & 8u;
    return c.ctrl == ctrl && c.shift == shift && c.alt == alt && c.win == win;
}

void PtEnter() {
    g_ptActive.store(true, std::memory_order_relaxed);
    g_ptStartTick = GetTickCount();
    g_ptFgPumpTick = 0;
    g_ptSawOtherFg = false;
    g_ptDownKeys.clear();
    g_moveTrack.valid = false;
}

void PtClose(const char* why) {
    g_ptActive.store(false, std::memory_order_relaxed);
    g_ptDownKeys.clear();
    g_moveTrack.valid = false;
    g_inputReset.store(1, std::memory_order_release);
    Log("[input] passthrough closed (%s)", why);
}

// Throttled foreground snapshot shared by both hooks (same thread). Returns
// true while the game window owns the foreground; g_ptSawOtherFg records
// "the user left the game at least once" for the borrow window's auto-close.
bool PtPump() {
    DWORD now = GetTickCount();
    if (now - g_ptFgPumpTick >= kPtFgPumpMs) {
        g_ptFgPumpTick = now;
        HWND game = (HWND)g_gameHwnd.load(std::memory_order_relaxed);
        HWND fg = GetForegroundWindow();
        bool nowOn = (!fg || fg == game);
        if (nowOn != g_ptOnGame)
            Log("[input] foreground %s game (fg=%p game=%p)", nowOn ? "on" : "off", (void*)fg, (void*)game);
        g_ptOnGame = nowOn;
        if (!g_ptOnGame) g_ptSawOtherFg = true;
    }
    return g_ptOnGame;
}

// Borrow window auto-close: the foreground came back to the game after the
// tool had it (capture finished/cancelled), or the grace window expired while
// the foreground never left the game (instant captures like NVIDIA's
// screenshot key do not open a window).
bool PtBorrowShouldClose() {
    PtPump();
    return g_ptOnGame && (g_ptSawOtherFg || GetTickCount() - g_ptStartTick >= kPtGraceMs);
}

// SendInput from inside a hook callback would re-enter the hook chain, so the
// replay is deferred to the hook thread's message loop.
void PtSendReplay() {
    const HotkeyChord& c = g_ptPendingReplay;
    if (!c.vk) return;
    INPUT in[10]{};
    int n = 0;
    auto key = [&](WORD vk, bool up) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = vk;
        in[n].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
        ++n;
    };
    if (c.ctrl) key(VK_CONTROL, false);
    if (c.shift) key(VK_SHIFT, false);
    if (c.alt) key(VK_MENU, false);
    if (c.win) key(VK_LWIN, false);
    key((WORD)c.vk, false);
    key((WORD)c.vk, true);
    if (c.win) key(VK_LWIN, true);
    if (c.alt) key(VK_MENU, true);
    if (c.shift) key(VK_SHIFT, true);
    if (c.ctrl) key(VK_CONTROL, true);
    SendInput(n, in, sizeof(INPUT));
    Log("[input] passthrough replayed the chord synthetically (%d events)", n);
}

void RehookNow() {
    std::lock_guard<std::mutex> lock(g_hookMtx);
    UninstallHooksLocked();
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

    PtTrackMods(vk, !up);   // chord matcher state; must run before any early-out

    if (vk == VK_LWIN || vk == VK_RWIN) return CallNextHookEx(g_kbHook, code, w, l);

    // H2: capture is only meaningful while the menu owns the input. Take the
    // visibility snapshot first and gate the capture branch on it, so a capture
    // flag left set after the menu closed can never swallow gameplay keys.
    static bool ptWasOpen = false;
    bool menuOpenNow = g_menuOpen.load(std::memory_order_relaxed);
    // Menu-open edge: fold in the modifiers held before the open (they were
    // passed while closed, so the async state is accurate for them).
    if (menuOpenNow && !ptWasOpen) g_ptMods |= PtAsyncMods();
    ptWasOpen = menuOpenNow;

    // Key capture flow: first non-modifier key completes the capture; the
    // tracked modifier state at that moment rides along (v2 chord captures
    // use it, single-key captures ignore it).
    if (menuOpenNow && g_captureMode.load(std::memory_order_relaxed) == 1) {
        if (!up && !IsModifier(vk)) {
            g_capturedVk.store(vk, std::memory_order_release);
            // M6: Win is passed through to the shell (see the early-out above),
            // so it can never be part of a usable chord; record only Ctrl/Shift
            // /Alt and let the hint text stop promising Win support.
            g_capturedMods.store(g_ptMods & 0x7u, std::memory_order_release);
            g_captureMode.store(0, std::memory_order_relaxed);
        }
        return 1;  // never let capture strokes reach the game
    }

    if (vk == g_toggleVk.load(std::memory_order_relaxed)) {
        if (g_ptActive.load(std::memory_order_relaxed)) PtClose("menu toggled");
        if (!up && !g_toggleWasDown) g_wantToggle.store(true, std::memory_order_release);
        g_toggleWasDown = !up;
        if (menuOpenNow) return 1;
        return CallNextHookEx(g_kbHook, code, w, l);  // closed: game also sees the key
    }

    if (!menuOpenNow) {
        if (g_ptActive.load(std::memory_order_relaxed)) PtClose("menu closed");
        // H6: registered mod hotkeys fire while the menu is closed, but only
        // through the render-thread queue - never inside the LL hook.
        if (RegistryQueueHotkey(vk, up, g_ptMods)) return 1;
        return CallNextHookEx(g_kbHook, code, w, l);
    }

    // Menu open. The swallow only applies while the game window owns the
    // foreground - Win / Alt-Tab / clicking another app must leave the shell's
    // own keyboard working (the overlay's watchdog reclaims hooks and raw
    // input when the game comes back). Esc still closes the menu from there.
    if (vk == VK_TAB && (GetAsyncKeyState(VK_MENU) & 0x8000))
        return CallNextHookEx(g_kbHook, code, w, l);

    PtPump();
    if (!g_ptOnGame) {
        if (vk == VK_ESCAPE) {
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
        return CallNextHookEx(g_kbHook, code, w, l);
    }

    // Screenshot-tool passthrough: a configured chord opens the borrow window
    // instead of feeding the menu. While it is open every key goes through so
    // PixPin/NVIDIA capture overlays keep working; the game camera stays
    // frozen because the game's raw mouse input still points at the overlay.
    if (!up && !IsModifier(vk)) {
        HotkeyChord hit{};
        bool matched = false, replay = false;
        AcquireSRWLockShared(&g_ptLock);
        for (const HotkeyChord& c : g_ptChords) {
            if (c.vk != vk) continue;
            if (PtModsMatch(c)) {
                hit = c;
                matched = true;
                replay = g_ptReplay;
                break;
            }
            if (!g_ptActive.load(std::memory_order_relaxed))
                Log("[input] chord vk hit, mods mismatch (want %s: tracked ctrl=%d shift=%d alt=%d win=%d)",
                    Config::ChordToString(c).c_str(),
                    (g_ptMods & 1u) ? 1 : 0, (g_ptMods & 2u) ? 1 : 0, (g_ptMods & 4u) ? 1 : 0, (g_ptMods & 8u) ? 1 : 0);
        }
        ReleaseSRWLockShared(&g_ptLock);
        if (matched && !g_ptActive.load(std::memory_order_relaxed)) {
            PtEnter();
            std::string nm = Config::ChordToString(hit);
            if (replay) {
                // Hook-based tools never saw the swallowed modifier-down, so
                // replay the whole chord synthetically; the injected events
                // come back through the active branch below.
                g_ptPendingReplay = hit;
                PostThreadMessageW(g_hookThreadId.load(std::memory_order_relaxed), WM_APP_PBZFM_REPLAY, 0, 0);
                Log("[input] passthrough entered: %s (replayed)", nm.c_str());
                return 1;
            }
            g_ptDownKeys.insert(vk);
            Log("[input] passthrough entered: %s", nm.c_str());
            return CallNextHookEx(g_kbHook, code, w, l);
        }
    }

    if (g_ptActive.load(std::memory_order_relaxed)) {
        if (PtBorrowShouldClose()) {
            bool letThrough = up && g_ptDownKeys.count(vk) != 0;
            PtClose("foreground back on the game");
            if (letThrough) return CallNextHookEx(g_kbHook, code, w, l);  // never strand a passed key
            return 1;
        }
        if (up) g_ptDownKeys.erase(vk);
        else g_ptDownKeys.insert(vk);
        return CallNextHookEx(g_kbHook, code, w, l);
    }

    // Registered mod hotkeys stay live while the menu is open (screenshot /
    // photo-mode toggles are meta-features, not game input). A dispatched key
    // is swallowed and does not reach the menu widgets either. H6: queued, the
    // callback runs on the render thread.
    if (RegistryQueueHotkey(vk, up, g_ptMods)) return 1;

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
    // Away from the game: the shell owns the mouse as well; rebase motion
    // tracking so the menu cursor jumps cleanly on return.
    PtPump();
    if (!g_ptOnGame) {
        g_moveTrack.valid = false;
        return CallNextHookEx(g_mouseHook, code, w, l);
    }
    // Passthrough borrow window: everything goes to the capture tool; when
    // the foreground is back on the game, resume isolation with this event.
    if (g_ptActive.load(std::memory_order_relaxed)) {
        if (PtBorrowShouldClose()) {
            PtClose("foreground back on the game");
            return 1;
        }
        return CallNextHookEx(g_mouseHook, code, w, l);
    }
    InputEvent e{};
    switch (w) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
            e.type = InputEvent::Btn; e.key = 0; e.down = (w == WM_LBUTTONDOWN); Push(e); return 1;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
            e.type = InputEvent::Btn; e.key = 1; e.down = (w == WM_RBUTTONDOWN); Push(e); return 1;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
            e.type = InputEvent::Btn; e.key = 2; e.down = (w == WM_MBUTTONDOWN); Push(e); return 1;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: {
            // B3: XBUTTON1/2 map to ImGui buttons 3/4. The old base 3 turned
            // XBUTTON2 into 5, one past ImGuiMouseButton_COUNT (5), indexing
            // MouseDown[5] out of bounds (the assert is compiled out in
            // Release). Down and up share this mapping, so they stay paired.
            unsigned xb = HIWORD(s->mouseData);
            if (xb != XBUTTON1 && xb != XBUTTON2) return 1;   // unknown side button: swallow only
            e.type = InputEvent::Btn; e.key = 2 + (int)xb; e.down = (w == WM_XBUTTONDOWN); Push(e); return 1;
        }
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
    g_hookThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(g_hookMtx);
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
        g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, GetModuleHandleW(nullptr), 0);
        Log("[input] hooks installed (kb=%p mouse=%p)", (void*)g_kbHook, (void*)g_mouseHook);
    }
    MSG msg;
    while (g_running.load(std::memory_order_relaxed) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_APP_PBZFM_REHOOK) { RehookNow(); continue; }
        if (msg.message == WM_APP_PBZFM_REPLAY) { PtSendReplay(); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    {
        std::lock_guard<std::mutex> lock(g_hookMtx);
        UninstallHooksLocked();
    }
    Log("[input] hooks removed");
    return 0;
}

} // namespace

bool InputStart() {
    g_hookThread = CreateThread(nullptr, 0, HookThread, nullptr, 0, nullptr);
    return g_hookThread != nullptr;
}

bool InputStop() {
    if (!g_hookThread) return true;
    // H9: unhook BEFORE joining. UnhookWindowsHookEx is process-wide and
    // synchronous, so once it returns the game's input chain no longer enters
    // our code even if the hook thread is wedged inside GetMessageW (which a
    // plain PostThreadMessage can miss). Order matters: join first would leave
    // the hooks live while we wait.
    {
        std::lock_guard<std::mutex> lock(g_hookMtx);
        UninstallHooksLocked();
    }
    const DWORD tid = g_hookThreadId.load(std::memory_order_relaxed);
    bool posted = false;
    for (int i = 0; i < 20 && !posted; ++i) {
        // The message queue may not exist yet right after InputStart; retry
        // briefly instead of silently leaving GetMessageW blocked forever.
        posted = tid != 0 && PostThreadMessageW(tid, WM_QUIT, 0, 0) != 0;
        if (!posted) Sleep(5);
    }
    if (!posted) Log("[input] WARNING: WM_QUIT could not be posted (tid=%lu err=%lu)", tid, GetLastError());
    if (WaitForSingleObject(g_hookThread, 3000) != WAIT_OBJECT_0) {
        // H9: never close the handle while the thread may still execute our
        // code - keeping it signals "not safe to unload" to the caller (which
        // pins the module instead of letting UE4SS FreeLibrary it).
        Log("[input] ERROR: hook thread still running after 3s - module must not be unloaded");
        return false;
    }
    CloseHandle(g_hookThread);
    g_hookThread = nullptr;
    g_hookThreadId.store(0, std::memory_order_relaxed);
    return true;
}

bool InputPop(InputEvent* out) {
    bool got = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_queue.empty()) {
            *out = g_queue.front();
            g_queue.pop_front();
            got = true;
        }
    }
    // L1: report drops from the render thread (at most one WARNING per 5 s) so
    // the hook callback never takes the log mutex.
    unsigned pending = g_queueDropPending.load(std::memory_order_relaxed);
    if (pending) {
        DWORD now = GetTickCount();
        DWORD last = g_queueDropLogTick.load(std::memory_order_relaxed);
        if (now - last >= 5000 && g_queueDropLogTick.compare_exchange_strong(last, now)) {
            pending = g_queueDropPending.exchange(0, std::memory_order_relaxed);
            Log("[input] WARNING: queue full - dropped %u event(s) (%u total this session)",
                pending, g_queueDropTotal.load(std::memory_order_relaxed));
        }
    }
    return got;
}

// L1: closing the menu ends an input session; the counters must not accumulate
// across sessions (a new session's first warning should reflect its own drops).
void InputResetDropStats() {
    g_queueDropPending.store(0, std::memory_order_relaxed);
    g_queueDropTotal.store(0, std::memory_order_relaxed);
    g_queueDropLogTick.store(0, std::memory_order_relaxed);
}

// L3: a session boundary leaves unread Move events in the queue (the frame that
// closed the menu never drained the tail). Clearing them makes the first cursor
// position of the next open come from the real GetCursorPos seed, not from a
// stale tail that would snap the cursor.
void InputClearPendingMoves() {
    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.erase(std::remove_if(g_queue.begin(), g_queue.end(),
        [](const InputEvent& e) { return e.type == InputEvent::Move; }),
        g_queue.end());
}

void InputPushEvent(const InputEvent& e) { Push(e); }

void InputSetPassthrough(const std::vector<HotkeyChord>& chords, bool replay) {
    {
        AcquireSRWLockExclusive(&g_ptLock);
        g_ptChords = chords;
        g_ptReplay = replay;
        ReleaseSRWLockExclusive(&g_ptLock);
    }
    std::string names;
    for (size_t i = 0; i < chords.size(); ++i) {
        if (i) names += ", ";
        names += Config::ChordToString(chords[i]);
    }
    Log("[input] passthrough chords: %s (replay=%d)", names.empty() ? "(none)" : names.c_str(), (int)replay);
}

bool InputPassthroughActive() { return g_ptActive.load(std::memory_order_relaxed); }

void InputRehook() {
    const DWORD tid = g_hookThreadId.load(std::memory_order_relaxed);
    if (tid) PostThreadMessageW(tid, WM_APP_PBZFM_REHOOK, 0, 0);
}

} // namespace pbzfm
