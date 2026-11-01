#pragma once
#include <windows.h>

namespace pbzfm {

// Low-level keyboard/mouse hooks. While the menu is open, all input is eaten
// at the system level (game gets nothing - same behavior as SKSE Menu
// Framework blocking windows) and forwarded to ImGui; mouse motion is not
// passed through either, the hook feeds its absolute position to the overlay
// which draws its own cursor. While closed, the only interception is the
// configurable toggle key and the key-capture flow.
//
// LL hooks run newest-first. The game's own input stack (or anti-cheat) can
// install its hooks AFTER ours and consume every mouse move before we see
// one - the menu cursor freezes while the camera keeps tracking. Re-hooking
// on menu open jumps our hooks back to the front of the chain.
bool InputStart();   // spawns the hook thread (returns immediately)
void InputStop();
void InputRehook();  // unhook + rehook on the hook thread (front of chain)

struct InputEvent {
    enum Type { Key, Btn, Wheel, Move } type;
    int   key;              // ImGuiKey (Key) or mouse button index (Btn)
    bool  down;
    float wx, wy;           // Wheel: wheel deltas in ImGui wheel units.
                            // Move: absolute screen position.
};

bool InputPop(InputEvent* out);   // render thread consumes the queue

// Test-only: enqueue a synthetic event as if the hook had produced it. The
// host test drives the menu through this when the live desktop cannot
// guarantee an uncontended input stream (a human moving the real mouse
// races the injected strokes).
void InputPushEvent(const InputEvent& e);

} // namespace pbzfm
