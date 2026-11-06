#pragma once
#include <windows.h>
#include <vector>

#include "config.h"

namespace pbzfm {

// Low-level keyboard/mouse hooks. While the menu is open, all input is eaten
// at the system level (game gets nothing - same behavior as SKSE Menu
// Framework blocking windows) and forwarded to ImGui; mouse motion is not
// passed through either, the hook feeds its absolute position to the overlay
// which draws its own cursor. While closed, the only interception is the
// configurable toggle key and the key-capture flow.
//
// The swallow only applies while the game window owns the foreground. Win /
// Alt-Tab / clicking another app must leave the shell's own cursor and
// keyboard working, so the hooks pass everything through while the user is
// away and rebase motion tracking when the game is front again.
//
// LL hooks run newest-first. The game's own input stack (or anti-cheat) can
// install its hooks AFTER ours and consume every mouse move before we see
// one - the menu cursor freezes while the camera keeps tracking. Re-hooking
// on menu open jumps our hooks back to the front of the chain.
bool InputStart();   // spawns the hook thread (returns immediately)
// H9: unhooks first, then posts WM_QUIT and waits up to 3 s. Returns false when
// the hook thread could not be joined - the caller must then treat the module
// as unsafe to unload (see RequestShutdown in pbzfm.cpp).
bool InputStop();
void InputRehook();  // unhook + rehook on the hook thread (front of chain)

struct InputEvent {
    enum Type { Key, Btn, Wheel, Move } type;
    int   key;              // ImGuiKey (Key) or mouse button index (Btn)
    bool  down;
    float wx, wy;           // Wheel: wheel deltas in ImGui wheel units.
                            // Move: absolute screen position.
};

bool InputPop(InputEvent* out);   // render thread consumes the queue
// L1: session-scoped drop counters (queue-full evictions); reset on open/close.
void InputResetDropStats();
// L3: drop unread Move events at a session boundary so a reopen never seeds the
// cursor from a previous session's queue tail.
void InputClearPendingMoves();

// Screenshot-tool passthrough. While the menu owns the input, the configured
// chords (PixPin's Alt+P, the NVIDIA App capture key, ...) open a borrow
// window instead of reaching the menu: keys and clicks pass through so the
// tool's capture overlay works, while the game camera stays frozen (raw input
// still points at the overlay) and the menu keeps rendering behind it. The
// window closes itself when the foreground returns to the game, or when a
// tool never takes the foreground (instant captures) after a grace period.
// Call before InputStart (the hook reads the list without further notice).
void InputSetPassthrough(const std::vector<HotkeyChord>& chords, bool replay);
bool InputPassthroughActive();   // test-only probe: is the borrow window open?

// Test-only: enqueue a synthetic event as if the hook had produced it. The
// host test drives the menu through this when the live desktop cannot
// guarantee an uncontended input stream (a human moving the real mouse
// races the injected strokes).
void InputPushEvent(const InputEvent& e);

// ---- 方案 C：窗口消息输入通道 ---------------------------------------------
// 引擎模式下不装任何全局钩子，菜单输入改由窗口消息提供：
//   * 键盘：游戏窗口仍是前台窗口，键消息进它的 WndProc（已子类化）→ 这里
//   * 鼠标：覆盖窗是 topmost 的真实窗口，指针下的消息进它的 WndProc → 这里
// 这些包装只做 VK/坐标转换，然后走同一条事件队列，ImGui 侧无需改动。
void InputPushKeyVk(unsigned vk, bool down);      // VK -> ImGuiKey；映射不到则丢弃
void InputPushMouseButton(int button, bool down); // 0=左 1=右 2=中 3/4=侧键
void InputPushWheel(float dy);                    // ImGui 滚轮单位
void InputPushMoveScreen(int screenX, int screenY);

} // namespace pbzfm
