// SHELVED (0.3.2): the gamepad feature is fully implemented but NOT built —
// it was pulled from the official package on request. To revive:
//   1. add src/gamepad.cpp back to the PBZFM target in CMakeLists.txt
//   2. overlay.cpp: include gamepad.h; GamepadStart(cfg) after InitImGui;
//      GamepadPoll(true, dt, seedSx, seedSy) at the top of RenderFrame (dt
//      computed first, seed = ImGui cursor converted to screen coords,
//      <= -1e9f when unknown); GamepadPoll(false, 0.030f, -2e9f, -2e9f) in
//      the render loop's closed branch; GamepadOnMenuOpen/OnMenuClose in
//      SetVisible; GamepadStop() at render-thread exit
//   3. input.h/.cpp: restore InputTakeCursorTrack + the hook-track rebase
//      (WM_MOUSEMOVE consumes a pending (x,y) rebase before accumulating)
//   4. config.h/.cpp: [gamepad] section (enabled/speed/deadzone/freeze_input/
//      open_combo/open_hold); menu.cpp settings block; i18n keys
//      gamepad/gamepad_open/gamepad_hold/gamepad_speed/gamepad_deadzone/
//      gamepad_help; host/main.cpp --gamepad mode + PBZFM_TestInjectGamepad/
//      TestGamepadClear exports
// Everything below is the original design documentation.
//
// XInput gamepad support for the menu (手柄适配).
#pragma once

namespace pbzfm {

struct Config;

// Reading: the render thread polls a dedicated xinput module and feeds the
// same InputEvent queue the low-level hooks write - left stick / D-pad move
// the virtual menu cursor, A/X/Y are mouse buttons, right stick + triggers
// scroll, Start opens the menu while it is closed, B/Start close it.
// The poll module is xinput9_1_0.dll by preference: the engine loads
// xinput1_4 first (UE5 XInputInterface order 1_4 -> 1_3 -> 9_1_0), so the
// module we keep enabled is the one the game is least likely to read.
//
// Freezing the game: XInput has no system-level hook, but XInputEnable(FALSE)
// makes a module report neutral state (buttons up, sticks centered) to ALL
// of its callers. While the menu is open every xinput module EXCEPT our read
// module is disabled, so the game's own polling sees a dead-centered pad -
// no inline hooks, no game-memory writes. Restored on close. Games reading
// the pad through Windows.Gaming.Input or DirectInput are NOT frozen by
// this; freeze_input = false in PBZFM.ini turns the whole dance off.
bool  GamepadStart(Config* cfg);   // load modules on the render thread
void  GamepadStop();               // unfreeze + release module refs
void  GamepadOnMenuOpen();         // reset cursor seed + close-button grace
void  GamepadOnMenuClose();
void  GamepadPoll(bool menuOpen, float dt, float seedSx, float seedSy);
                                   // seed: current ImGui cursor position in
                                   // screen coords; <= -1e9 when unknown

// Test-only: force the poll source to a fixed state (host --gamepad mode).
// While active it replaces the real controller entirely.
void  GamepadSetSynthetic(bool active, unsigned short buttons,
                          short lx, short ly, short rx, short ry,
                          unsigned char lt, unsigned char rt);

} // namespace pbzfm
