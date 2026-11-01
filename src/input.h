#pragma once
#include <windows.h>

namespace pbzfm {

// Low-level keyboard/mouse hooks. While the menu is open, all input is eaten
// at the system level (game gets nothing - same behavior as SKSE Menu
// Framework blocking windows) and forwarded to ImGui; while closed, the only
// interception is the configurable toggle key and the key-capture flow.
bool InputStart();   // spawns the hook thread (returns immediately)
void InputStop();

struct InputEvent {
    enum Type { Key, Btn, Wheel } type;
    int   key;              // ImGuiKey (Key) or mouse button index (Btn)
    bool  down;
    float wx, wy;           // wheel deltas, in ImGui wheel units
};

bool InputPop(InputEvent* out);   // render thread consumes the queue

} // namespace pbzfm
