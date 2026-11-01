#include "gamepad.h"

#include "common.h"
#include "config.h"
#include "input.h"

#include <xinput.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace pbzfm {

namespace {

typedef void  (WINAPI *PFN_XInputEnable)(BOOL);
typedef DWORD (WINAPI *PFN_XInputGetState)(DWORD, XINPUT_STATE*);

struct XInputModule {
    HMODULE lib = nullptr;
    PFN_XInputEnable enable = nullptr;
    PFN_XInputGetState getState = nullptr;
    const char* name = "";
};

XInputModule g_mod[3];          // 0: xinput1_4, 1: xinput1_3, 2: xinput9_1_0
int          g_readIdx = -1;    // our poll source (kept enabled)
bool         g_started = false;
bool         g_frozen = false;
Config*      g_cfg = nullptr;

// Open-combo options (index == Config::gpOpenCombo). While the menu is
// closed the game receives every button normally, so the summon combo must
// avoid game-owned inputs: START opens the game's pause menu, and LB/RB are
// common skill bindings. A long-pressed BACK is the least intrusive default;
// the duration is configurable (open_hold, 0 = instant).
const WORD kOpenComboMask[4] = {
    XINPUT_GAMEPAD_BACK,
    XINPUT_GAMEPAD_START,
    XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER,
    XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB,
};

int g_vsX = 0, g_vsY = 0, g_vsW = 0, g_vsH = 0;   // virtual screen bounds

// Test-only synthetic state (host thread writes, render thread reads).
std::mutex g_synthMutex;
bool  g_synthActive = false;
WORD  g_synthButtons = 0;
SHORT g_synthLx = 0, g_synthLy = 0, g_synthRx = 0, g_synthRy = 0;
BYTE  g_synthLt = 0, g_synthRt = 0;

// Menu-open runtime. prevButtons is kept across the open transition: the
// Start press that opened the menu is still down during the first open
// frames, and a fresh rising edge there would close it immediately.
struct PadState {
    bool  havePos = false;
    float px = 0.0f, py = 0.0f;   // virtual cursor, screen coords
    WORD  prevButtons = 0;
    float grace = 0.0f;           // seconds; B/Start edges ignored after open
    float openHold = 0.0f;        // seconds the open combo was continuously held
    bool  openLatch = false;      // one trigger per physical hold
};
PadState g_st;

bool ReadState(XINPUT_STATE* out) {
    {
        std::lock_guard<std::mutex> lock(g_synthMutex);
        if (g_synthActive) {
            out->dwPacketNumber = 1;
            out->Gamepad.wButtons = g_synthButtons;
            out->Gamepad.bLeftTrigger = g_synthLt;
            out->Gamepad.bRightTrigger = g_synthRt;
            out->Gamepad.sThumbLX = g_synthLx;
            out->Gamepad.sThumbLY = g_synthLy;
            out->Gamepad.sThumbRX = g_synthRx;
            out->Gamepad.sThumbRY = g_synthRy;
            return true;
        }
    }
    if (g_readIdx < 0) return false;
    for (DWORD user = 0; user < 4; ++user) {
        if (g_mod[g_readIdx].getState(user, out) == ERROR_SUCCESS) return true;
    }
    return false;
}

void ApplyFreeze(bool frozen) {
    if (frozen == g_frozen) return;
    if (frozen) {
        int n = 0;
        for (int i = 0; i < 3; ++i) {
            if (i == g_readIdx || !g_mod[i].enable) continue;
            g_mod[i].enable(FALSE);
            ++n;
        }
        Log("[gp] XInputEnable(FALSE) on %d module(s), read module %s kept live",
            n, g_readIdx >= 0 ? g_mod[g_readIdx].name : "(none)");
    } else {
        for (int i = 0; i < 3; ++i) {
            if (i == g_readIdx || !g_mod[i].enable) continue;
            g_mod[i].enable(TRUE);
        }
        Log("[gp] XInputEnable(TRUE) restored");
    }
    g_frozen = frozen;
}

// Radial deadzone + squared response, direction preserved. In: normalized
// [-1,1] axes; out: response in [0,1] per axis.
void StickResponse(float nx, float ny, float dz, float* ox, float* oy) {
    float mag = sqrtf(nx * nx + ny * ny);
    if (mag <= dz || mag <= 0.0001f) { *ox = *oy = 0.0f; return; }
    float t = (mag - dz) / (1.0f - dz);
    float s = (t * t) / mag;
    *ox = nx * s;
    *oy = ny * s;
}

float TriggerNorm(BYTE v) {
    const float dz = 40.0f;
    if ((float)v <= dz) return 0.0f;
    return ((float)v - dz) / (255.0f - dz);
}

void PushBtn(int idx, bool down) {
    InputEvent e{};
    e.type = InputEvent::Btn;
    e.key = idx;
    e.down = down;
    InputPushEvent(e);
}

} // namespace

bool GamepadStart(Config* cfg) {
    g_cfg = cfg;
    static const struct { const wchar_t* dll; const char* name; } kCandidates[3] = {
        { L"xinput1_4.dll",   "xinput1_4" },
        { L"xinput1_3.dll",   "xinput1_3" },
        { L"xinput9_1_0.dll", "xinput9_1_0" },
    };
    for (int i = 0; i < 3; ++i) {
        g_mod[i].name = kCandidates[i].name;
        g_mod[i].lib = LoadLibraryW(kCandidates[i].dll);
        if (!g_mod[i].lib) continue;
        g_mod[i].enable = (PFN_XInputEnable)GetProcAddress(g_mod[i].lib, "XInputEnable");
        g_mod[i].getState = (PFN_XInputGetState)GetProcAddress(g_mod[i].lib, "XInputGetState");
    }
    static const int kReadPreference[3] = { 2, 1, 0 };   // 9_1_0 -> 1_3 -> 1_4
    g_readIdx = -1;
    for (int p = 0; p < 3; ++p) {
        int i = kReadPreference[p];
        if (g_mod[i].lib && g_mod[i].getState) { g_readIdx = i; break; }
    }
    g_started = g_readIdx >= 0;
    g_vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (g_started) {
        Log("[gp] ready: poll via %s, freeze on menu open via XInputEnable (freeze_input=%s)",
            g_mod[g_readIdx].name, cfg && cfg->gpFreeze ? "on" : "off");
    } else {
        Log("[gp] no xinput module loadable - gamepad support inactive");
    }
    return g_started;
}

void GamepadStop() {
    ApplyFreeze(false);
    for (int i = 0; i < 3; ++i) {
        if (g_mod[i].lib) FreeLibrary(g_mod[i].lib);
        g_mod[i].lib = nullptr;
        g_mod[i].enable = nullptr;
        g_mod[i].getState = nullptr;
    }
    g_readIdx = -1;
    g_started = false;
}

void GamepadOnMenuOpen() {
    g_st.havePos = false;
    g_st.grace = 0.25f;
}

void GamepadOnMenuClose() {
    ApplyFreeze(false);
}

void GamepadPoll(bool menuOpen, float dt, float seedSx, float seedSy) {
    if (!g_cfg) return;

    // Freeze state follows the live config, so toggling the gamepad on the
    // settings page while the menu is open cannot leave the game stuck
    // disabled (or the pad live to both us and the game).
    const bool enabled = g_cfg->gpEnabled;
    ApplyFreeze(menuOpen && enabled && g_cfg->gpFreeze);

    XINPUT_STATE st{};
    const bool hasPad = ReadState(&st);

    if (!menuOpen) {
        // Closed menu: watch the configured open combo (default: hold BACK).
        if (!enabled || !hasPad) {
            g_st.openHold = 0.0f;
            g_st.openLatch = false;
            g_st.prevButtons = 0;
            return;
        }
        const WORD mask = kOpenComboMask[g_cfg->gpOpenCombo];
        if ((st.Gamepad.wButtons & mask) == mask) {
            g_st.openHold += dt;
            if (!g_st.openLatch && g_st.openHold >= g_cfg->gpOpenHold) {
                g_st.openLatch = true;   // one trigger per physical hold
                g_wantToggle.store(true, std::memory_order_release);
            }
        } else {
            g_st.openHold = 0.0f;
            g_st.openLatch = false;
        }
        g_st.prevButtons = st.Gamepad.wButtons;
        return;
    }

    if (!enabled || !hasPad) {
        // Controller gone mid-press (or the host test cleared the synthetic
        // channel): emit the missing mouse-button ups so ImGui does not hold
        // a stuck button - a click never completes without its release.
        const WORD released = g_st.prevButtons &
            (XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y);
        if (released & XINPUT_GAMEPAD_A) PushBtn(0, false);
        if (released & XINPUT_GAMEPAD_X) PushBtn(1, false);
        if (released & XINPUT_GAMEPAD_Y) PushBtn(2, false);
        g_st.prevButtons = 0;
        return;
    }

    const WORD btn = st.Gamepad.wButtons;
    const WORD rising = btn & ~g_st.prevButtons;
    const WORD falling = g_st.prevButtons & ~btn;
    g_st.prevButtons = btn;

    if (g_st.grace > 0.0f) g_st.grace -= dt;

    // B / Start close the menu, except during the grace window right after
    // open (the press that opened it may still read as held).
    if (g_st.grace <= 0.0f &&
        (rising & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START))) {
        g_closeRequest.store(true, std::memory_order_release);
    }

    // Mouse buttons: A left, X right, Y middle.
    if (rising & XINPUT_GAMEPAD_A) PushBtn(0, true);
    if (falling & XINPUT_GAMEPAD_A) PushBtn(0, false);
    if (rising & XINPUT_GAMEPAD_X) PushBtn(1, true);
    if (falling & XINPUT_GAMEPAD_X) PushBtn(1, false);
    if (rising & XINPUT_GAMEPAD_Y) PushBtn(2, true);
    if (falling & XINPUT_GAMEPAD_Y) PushBtn(2, false);

    // Cursor motion from the left stick (quadratic response) and D-pad.
    const float maxSpeed = 1600.0f * g_cfg->gpSpeed;
    float ax = 0.0f, ay = 0.0f;
    StickResponse((float)st.Gamepad.sThumbLX / 32767.0f,
                  (float)st.Gamepad.sThumbLY / 32767.0f,
                  g_cfg->gpDeadzone, &ax, &ay);
    float vx = ax * maxSpeed;
    float vy = ay * maxSpeed;
    const float dpad = 0.5f * maxSpeed;
    if (btn & XINPUT_GAMEPAD_DPAD_LEFT)  vx -= dpad;
    if (btn & XINPUT_GAMEPAD_DPAD_RIGHT) vx += dpad;
    if (btn & XINPUT_GAMEPAD_DPAD_UP)    vy -= dpad;
    if (btn & XINPUT_GAMEPAD_DPAD_DOWN)  vy += dpad;

    if (fabsf(vx) > 0.5f || fabsf(vy) > 0.5f) {
        if (!g_st.havePos) {
            if (seedSx <= -1.0e9f) return;   // wait for a valid cursor seed
            g_st.px = seedSx;
            g_st.py = seedSy;
            g_st.havePos = true;
        }
        g_st.px = std::clamp(g_st.px + vx * dt,
                             (float)g_vsX + 1.0f, (float)(g_vsX + g_vsW) - 1.0f);
        g_st.py = std::clamp(g_st.py + vy * dt,
                             (float)g_vsY + 1.0f, (float)(g_vsY + g_vsH) - 1.0f);
        InputEvent e{};
        e.type = InputEvent::Move;
        e.wx = g_st.px;
        e.wy = g_st.py;
        InputPushEvent(e);
        // The hook's motion tracker still accumulates from its own frozen
        // position; re-anchor it so a real mouse move continues from where
        // the stick left the cursor instead of snapping back.
        InputTakeCursorTrack(g_st.px, g_st.py);
    }

    // Wheel: right stick (up = scroll up) + triggers (RT down, LT up).
    float wxr = 0.0f, wyr = 0.0f;
    StickResponse((float)st.Gamepad.sThumbRX / 32767.0f,
                  (float)st.Gamepad.sThumbRY / 32767.0f,
                  0.25f, &wxr, &wyr);
    const float wy = -wyr * 12.0f +
        (TriggerNorm(st.Gamepad.bLeftTrigger) - TriggerNorm(st.Gamepad.bRightTrigger)) * 16.0f;
    const float wx = wxr * 12.0f;
    if (fabsf(wx) > 0.0001f || fabsf(wy) > 0.0001f) {
        InputEvent e{};
        e.type = InputEvent::Wheel;
        e.wx = wx * dt;
        e.wy = wy * dt;
        InputPushEvent(e);
    }
}

void GamepadSetSynthetic(bool active, unsigned short buttons,
                         short lx, short ly, short rx, short ry,
                         unsigned char lt, unsigned char rt) {
    std::lock_guard<std::mutex> lock(g_synthMutex);
    g_synthActive = active;
    g_synthButtons = buttons;
    g_synthLx = lx; g_synthLy = ly;
    g_synthRx = rx; g_synthRy = ry;
    g_synthLt = lt; g_synthRt = rt;
}

} // namespace pbzfm
