// 方案 C：引擎内输入端口（engine port）
//
// 目的：把「冻结游戏 + 取菜单输入」从 OS 层（全局 LL 钩子 + Raw Input 改指向）
// 换成引擎层（UE4SS 反射调用 APlayerController 的输入 API）。引擎层天然只影响
// 本进程，系统输入流完全不被拦截 —— 录屏 / 截图 / Game Bar / 任何全局热键恢复
// 正常工作。
//
// 本头文件不包含任何引擎头：端口是运行期探测的，未编译引擎后端时（host 测试、
// 玩家没装 UE4SS）自动回退到现有 OS 钩子路径，行为与旧版一致。
#pragma once

#include <windows.h>

namespace pbzfm {

// PBZFM.ini [framework] input_mode 的三个取值。
enum class InputMode {
    Auto,    // 引擎端口可用就用引擎，否则 OS 钩子（默认）
    Engine,  // 强制引擎；不可用则告警并回退 OS 钩子
    Os,      // 强制旧路径（全局 LL 钩子 + Raw Input 改指向）
};

// 实际生效的后端。
enum class InputBackend {
    Os,
    Engine,
};

const char* InputModeName(InputMode m);            // "auto" / "engine" / "os"
InputMode   InputModeFromString(const char* s);    // 未知值 -> Auto

// 纯判定，便于单测：想用哪种后端 × 引擎是否就绪 -> 实际用哪种。
InputBackend ResolveInputBackend(InputMode wanted, bool engineReady);
const char*  InputBackendName(InputBackend b);     // "os" / "engine"

// ---- 引擎后端契约 ----------------------------------------------------------
// 由 engine_port_ue4ss.cpp 实现（CMake 选项 PBZFM_ENGINE_PORT=ON 时编译进来）。
// init 在模块初始化线程调用一次；返回 false 时 why 给出可读原因（写进日志）。
// menuOpen 由渲染线程在菜单开/关时调用，实现方必须自己保证线程安全（通常只是
// 存一个原子标志，真正的引擎调用放在游戏线程的 tick 钩子里执行）。
struct EngineBackend {
    const char* name;                            // "ue4ss"
    bool (*init)(const char** why);
    void (*shutdown)();
    void (*menuOpen)(bool open);
};

// ---- 框架侧状态机 ----------------------------------------------------------
// 在 GameWindow::Find() 成功之后、OverlayStart() 之前调用。
// 返回 true 表示引擎后端接管输入（调用方不得再装 LL 钩子 / 不得再改 Raw Input）。
bool EnginePortInit(InputMode wanted);
void EnginePortShutdown();

bool        EnginePortActive();          // 引擎后端是否接管
const char* EnginePortName();            // "ue4ss" / "unavailable"
const char* EnginePortStatus();          // 一行可读状态，供日志/自检页
const char* EnginePortUnavailableReason();

// 菜单开/关：引擎模式下转发给后端（冻结 look/move、显示光标、切 UI 输入模式）。
// OS 模式下它是空操作。
void EnginePortSetMenuOpen(bool open);

} // namespace pbzfm
