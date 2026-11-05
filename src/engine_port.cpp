// 方案 C 的框架侧状态机：选择输入后端、探测引擎能力、把菜单开/关转发给后端。
// 这里刻意不放任何引擎调用，保证 host 测试和「没装 UE4SS」的玩家行为不变。
#include "engine_port.h"
#include "common.h"

#include <atomic>
#include <cstring>
#include <string>

namespace pbzfm {

#if PBZFM_ENGINE_PORT_UE4SS
// 定义在 engine_port_ue4ss.cpp：只有编译了引擎后端时才有。
// 必须放在 pbzfm 命名空间（不是匿名命名空间），否则链接名对不上。
const EngineBackend* EngineBackendUe4ss();
#endif

namespace {

std::atomic<bool> g_active{ false };
std::atomic<bool> g_ready{ false };   // 后端 init() 成功过

// 只在初始化线程写、其它线程只读；用固定缓冲避免静态析构与日志重入。
char g_status[192] = "not initialised";
char g_reason[192] = "engine port not probed";

const EngineBackend* EngineBackendGet() {
#if PBZFM_ENGINE_PORT_UE4SS
    if (const EngineBackend* b = EngineBackendUe4ss()) return b;
#endif
    return nullptr;
}

void SetReason(const char* text) {
    if (!text) text = "";
    strncpy_s(g_reason, sizeof(g_reason), text, _TRUNCATE);
}

} // namespace

const char* InputModeName(InputMode m) {
    switch (m) {
        case InputMode::Engine: return "engine";
        case InputMode::Os: return "os";
        case InputMode::Auto:
        default: return "auto";
    }
}

InputMode InputModeFromString(const char* s) {
    if (!s) return InputMode::Auto;
    if (_stricmp(s, "engine") == 0 || _stricmp(s, "ue4ss") == 0) return InputMode::Engine;
    if (_stricmp(s, "os") == 0 || _stricmp(s, "hook") == 0 || _stricmp(s, "hooks") == 0) return InputMode::Os;
    return InputMode::Auto;
}

InputBackend ResolveInputBackend(InputMode wanted, bool engineReady) {
    if (wanted == InputMode::Os) return InputBackend::Os;
    // Auto 与 Engine 都要求引擎就绪；Engine 模式不就绪时由调用方告警后回退，
    // 仍然返回 Os —— 判定函数本身不产生副作用，便于单测。
    return engineReady ? InputBackend::Engine : InputBackend::Os;
}

const char* InputBackendName(InputBackend b) {
    return b == InputBackend::Engine ? "engine" : "os";
}

bool EnginePortActive() {
    return g_active.load(std::memory_order_acquire);
}

const char* EnginePortName() {
    if (!g_active.load(std::memory_order_acquire)) return "unavailable";
    const EngineBackend* b = EngineBackendGet();
    return b && b->name ? b->name : "unavailable";
}

const char* EnginePortStatus() {
    return g_status;
}

const char* EnginePortUnavailableReason() {
    return g_reason;
}

bool EnginePortInit(InputMode wanted) {
    const EngineBackend* backend = EngineBackendGet();
    bool ready = false;

    if (!backend) {
        SetReason("this build has no engine backend (PBZFM_ENGINE_PORT=OFF), or UE4SS is not present");
    } else {
        const char* why = "engine probe failed";
        ready = backend->init ? backend->init(&why) : false;
        SetReason(ready ? "engine backend ready" : (why ? why : "engine probe failed"));
    }

    const InputBackend chosen = ResolveInputBackend(wanted, ready);
    g_ready.store(ready, std::memory_order_release);
    g_active.store(chosen == InputBackend::Engine, std::memory_order_release);

    if (chosen == InputBackend::Engine) {
        snprintf(g_status, sizeof(g_status), "engine/%s (input_mode=%s, no OS hooks, no raw-input hijack)",
                 backend && backend->name ? backend->name : "?", InputModeName(wanted));
        Log("[engine] %s", g_status);
        return true;
    }

    if (wanted == InputMode::Engine && !ready) {
        // 用户显式要求引擎模式却不可用：必须说清楚，否则会以为是「引擎模式下录屏没修好」。
        snprintf(g_status, sizeof(g_status), "os fallback (input_mode=engine but %s)", g_reason);
        Log("[engine] WARNING: input_mode=engine but the engine port is unavailable - falling back to OS hooks (%s)",
            g_reason);
    } else {
        snprintf(g_status, sizeof(g_status), "os (input_mode=%s, %s)", InputModeName(wanted), g_reason);
        Log("[engine] %s", g_status);
    }
    return false;
}

void EnginePortShutdown() {
    if (g_ready.exchange(false, std::memory_order_acq_rel)) {
        if (const EngineBackend* b = EngineBackendGet()) {
            if (b->menuOpen) b->menuOpen(false);
            if (b->shutdown) b->shutdown();
        }
    }
    g_active.store(false, std::memory_order_release);
}

void EnginePortSetMenuOpen(bool open) {
    if (!g_active.load(std::memory_order_acquire)) return;
    if (const EngineBackend* b = EngineBackendGet()) {
        if (b->menuOpen) b->menuOpen(open);
    }
}

void EnginePortReassert() {
    if (!g_active.load(std::memory_order_acquire)) return;
    if (const EngineBackend* b = EngineBackendGet()) {
        if (b->reassert) b->reassert();
    }
}

} // namespace pbzfm
