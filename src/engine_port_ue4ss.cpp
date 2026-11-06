// 方案 C 的引擎后端：只在 PBZFM_ENGINE_PORT=ON（链接 UE4SS）时编译。
//
// 职责边界（很重要）：
//   * 引擎后端只负责「冻结这个进程的输入」和「把光标交还给系统」——
//     即 bBlockInput / SetIgnoreMoveInput / SetIgnoreLookInput / bShowMouseCursor。
//     这些调用天然只影响本进程，系统输入流一个字节都不碰，所以录屏/截图/
//     游戏栏/Steam 的全局热键在菜单打开时依然有效。
//   * 它**不负责**菜单的鼠标/键盘事件采集。本作是 Enhanced Input，PBZPhotoMode
//     实测 PlayerController 上没有可钩的 InputKey/InputAxis；菜单输入仍由覆盖窗
//     自己的窗口消息 + 游戏窗口子类提供（方案 A 的输入通道），所以本文件不装任何
//     全局 LL 钩子、不改 Raw Input。
//
// 线程模型：OnMenuOpen() 只写一个原子请求；所有引擎对象与 ProcessEvent 都在
// UE4SS 的 EngineTickPre 回调里（游戏线程）执行。渲染线程永远不碰 UObject。
#ifdef PBZFM_ENGINE_PORT_UE4SS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

// 顺序不能动：fmt 必须在 Unreal 头之前解析。
// Unreal/Core/Windows/WindowsPlatformCompilerSetup.hpp 把 4582/4583 提升为 error，
// 而 fmt 的 get_locale 匿名 union 恰好触发这两条；先让 fmt 走一遍 include guard，
// 后面再进 Unreal 头就不会重解析（PBZPhotoMode 的 ue4ss_port_internal.h 同样处理）。
#include <DynamicOutput/DynamicOutput.hpp>

#include <Unreal/Hooks.hpp>
#include <Unreal/NameTypes.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <Unreal/CoreUObject/UObject/Class.hpp>
#include <Unreal/CoreUObject/UObject/UnrealType.hpp>

#include "common.h"
#include "engine_port.h"

#include <Windows.h>

#include <atomic>
#include <string>
#include <vector>

namespace pbzfm {
namespace {

using namespace RC::Unreal;

// 0 = 无请求；1 = 菜单打开（冻结）；-1 = 菜单关闭（恢复）；2 = 重新施加冻结
std::atomic<int> g_request{ 0 };
bool g_frozen = false;          // 游戏线程私有
bool g_shutdown = false;
bool g_loggedFailure = false;
ULONGLONG g_lastEnsureMs = 0;   // 上次读回校验的时间（兜底轮询用）

UObject* g_pc = nullptr;        // 仅游戏线程访问
UObject* g_pawn = nullptr;      // 当前被我们屏蔽过 bBlockInput 的 Pawn（换代要重写）
bool g_blockWrotePc = false, g_blockOrigPc = false;
bool g_blockWrotePawn = false, g_blockOrigPawn = false;
bool g_cursorWrote = false, g_cursorOrig = false;
bool g_ignoreWrote = false;

Hook::GlobalCallbackId g_tickId = Hook::ERROR_ID;

// ------------------------------------------------------------ 基础工具（无 SEH）
// UE4SS 的 RC::StringType 是 std::wstring；参数名都是 ASCII 常量，直接扩宽即可。
RC::StringType WidenAscii(const char* text) {
    RC::StringType out;
    if (text == nullptr) return out;
    for (const char* p = text; *p != '\0'; ++p) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
    return out;
}

// ------------------------------------------------------------ SEH 叶子函数
// 铁律：__try 只能出现在「不含任何带析构局部对象」的叶子函数里，
// 否则 /EHsc 会报 C2712（PBZPhotoMode 的 internal.h 也是同样的拆法）。

__declspec(noinline) bool RawIsOk(UObject* o) {
    __try {
        const FUObjectItem* item = o->GetObjectItem();
        if (item == nullptr || item->GetUObject() != o || !item->IsValid(true)) return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

__declspec(noinline) UClass* RawGetClass(UObject* o) {
    __try { return o->GetClassPrivate(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline) UFunction* RawGetFunction(UObject* o, const wchar_t* name) {
    __try { return o->GetFunctionByNameInChain(name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// UObject/UStruct 自带的按名查找（沿继承链找 FProperty）。比 TFieldIterator 干净：
// 迭代器在 /EHsc 下会让 __try 触发 C2712，而这里只有一个直接被调用的成员函数。
__declspec(noinline) FProperty* RawGetPropertyByName(UObject* owner, const wchar_t* name) {
    __try { return owner->GetPropertyByNameInChain(name); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

__declspec(noinline) int32 RawStructureSize(UFunction* fn) {
    __try { return fn->GetStructureSize(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

__declspec(noinline) void RawProcessEvent(UObject* o, UFunction* fn, void* params) {
    __try { o->ProcessEvent(fn, params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* native 函数在参数失效时会自己读 0xFFFF... */ }
}

__declspec(noinline) void RawFindAllPlayerControllers(std::vector<UObject*>& out) {
    __try { UObjectGlobals::FindAllOf(L"PlayerController", out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { out.clear(); }
}

// ------------------------------------------------------------ 反射小工具（无 SEH）
inline bool Plausible(const void* p) {
    const auto value = reinterpret_cast<uintptr_t>(p);
    return value >= 0x10000 && value < 0x0000800000000000ULL && (value & 0x7) == 0;
}

bool IsOk(UObject* o) {
    if (!Plausible(o)) return false;
    UClass* cls = RawGetClass(o);
    if (!Plausible(cls)) return false;
    return RawIsOk(o);
}

FProperty* FindProperty(UObject* owner, const char* name) {
    if (!Plausible(owner)) return nullptr;
    const RC::StringType wide = WidenAscii(name);
    return RawGetPropertyByName(owner, wide.c_str());
}

// 读一个对象属性（Pawn / Player 这类 FObjectProperty 存的是 UObject*）
UObject* ReadObjectProperty(UObject* owner, const char* name) {
    FProperty* prop = FindProperty(owner, name);
    if (prop == nullptr) return nullptr;
    void* address = prop->ContainerPtrToValuePtr<void>(owner);
    if (address == nullptr) return nullptr;
    return *static_cast<UObject**>(address);
}

// 写一个 bool 属性；prev 非空时回读旧值（恢复用）
bool WriteBool(UObject* owner, const char* name, bool value, bool* prev) {
    FProperty* prop = FindProperty(owner, name);
    if (prop == nullptr || !prop->IsA<FBoolProperty>()) return false;
    auto* boolProp = static_cast<FBoolProperty*>(prop);
    if (prev != nullptr) *prev = boolProp->GetPropertyValueInContainer(owner);
    boolProp->SetPropertyValueInContainer(owner, value);
    return true;
}

// 读一个 bool 属性（用于判断游戏有没有把我们的冻结重置掉）
bool ReadBool(UObject* owner, const char* name, bool& out) {
    FProperty* prop = FindProperty(owner, name);
    if (prop == nullptr || !prop->IsA<FBoolProperty>()) return false;
    out = static_cast<FBoolProperty*>(prop)->GetPropertyValueInContainer(owner);
    return true;
}

// 调 void SetXxxInput(bool)：按参数名找属性、写值、ProcessEvent
bool InvokeBoolSetter(UObject* owner, const char* functionName, const char* paramName, bool value) {
    if (!IsOk(owner)) return false;
    const RC::StringType wideFn = WidenAscii(functionName);
    UFunction* fn = RawGetFunction(owner, wideFn.c_str());
    if (fn == nullptr) return false;

    const RC::StringType wideParam = WidenAscii(paramName);
    FProperty* param = RawGetPropertyByName(fn, wideParam.c_str());
    if (param == nullptr || !param->IsA<FBoolProperty>()) return false;

    const int32 size = RawStructureSize(fn);
    std::vector<uint8_t> buffer(static_cast<size_t>(size > 0 ? size : 1), 0);
    static_cast<FBoolProperty*>(param)->SetPropertyValueInContainer(buffer.data(), value);
    RawProcessEvent(owner, fn, buffer.data());
    return true;
}

// 真正在玩的那个 PlayerController 一定同时有 Player(ULocalPlayer) 和 Pawn；
// FindFirstOf 会拿到过场/spectator/已失效实例（PBZPhotoMode 实机结论）。
UObject* FindPlayerController() {
    std::vector<UObject*> candidates;
    RawFindAllPlayerControllers(candidates);

    UObject* fallback = nullptr;
    for (UObject* candidate : candidates) {
        if (!IsOk(candidate)) continue;
        if (fallback == nullptr) fallback = candidate;
        const bool hasPlayer = Plausible(ReadObjectProperty(candidate, "Player"));
        const bool hasPawn = Plausible(ReadObjectProperty(candidate, "Pawn"));
        if (hasPlayer && hasPawn) return candidate;
    }
    return fallback;
}

void LogFailureOnce(const char* what) {
    if (g_loggedFailure) return;
    g_loggedFailure = true;
    Log("[engine] %s failed - engine freeze disabled (menu input itself keeps working)", what);
}

// ------------------------------------------------------------ 冻结/恢复
bool Freeze() {
    if (g_frozen) return true;
    if (!IsOk(g_pc)) g_pc = FindPlayerController();
    if (!IsOk(g_pc)) {
        LogFailureOnce("no live PlayerController");
        return false;
    }

    // 三层屏蔽（PBZPhotoMode 实机结论）：bBlockInput 挡键鼠动作（已验证有效），
    // SetIgnoreMoveInput 挡移动轴，SetIgnoreLookInput 挡视角轴。
    // 只记录自己写成功的位，恢复时按位还原，不覆盖游戏自己设的值。
    g_blockWrotePc = WriteBool(g_pc, "bBlockInput", true, &g_blockOrigPc);
    UObject* pawn = ReadObjectProperty(g_pc, "Pawn");
    g_pawn = IsOk(pawn) ? pawn : nullptr;
    if (g_pawn != nullptr) g_blockWrotePawn = WriteBool(g_pawn, "bBlockInput", true, &g_blockOrigPawn);
    const bool move = InvokeBoolSetter(g_pc, "SetIgnoreMoveInput", "bNewMoveInput", true);
    const bool look = InvokeBoolSetter(g_pc, "SetIgnoreLookInput", "bNewLookInput", true);
    g_ignoreWrote = move || look;
    g_cursorWrote = WriteBool(g_pc, "bShowMouseCursor", true, &g_cursorOrig);

    if (!g_blockWrotePc && !g_ignoreWrote) {
        LogFailureOnce("bBlockInput / SetIgnore*Input");
        return false;
    }
    g_frozen = true;
    g_lastEnsureMs = GetTickCount64();
    Log("[engine] menu open - frozen (bBlockInput=%d ignore=%d cursor=%d)",
        (int)g_blockWrotePc, (int)g_ignoreWrote, (int)g_cursorWrote);
    return true;
}

// 读回校验 + 补写：游戏在窗口重新激活时会重建输入栈（本作实机：N 卡浮层抢焦点
// 回来后左键漏给 PBZPhotoMode），所以冻结不能只做一次。
void EnsureFrozen() {
    if (!g_frozen) return;
    g_lastEnsureMs = GetTickCount64();

    if (!IsOk(g_pc)) {
        // PC 换代/失效：旧指针上写的位已经没有意义，按新对象重新冻结。
        g_pc = FindPlayerController();
        if (!IsOk(g_pc)) return;
        g_blockWrotePc = g_blockWrotePawn = g_cursorWrote = g_ignoreWrote = false;
        g_pawn = nullptr;
        g_frozen = false;
        Freeze();
        return;
    }

    bool blocked = false;
    if (g_blockWrotePc && ReadBool(g_pc, "bBlockInput", blocked) && !blocked) {
        WriteBool(g_pc, "bBlockInput", true, nullptr);
        Log("[engine] re-asserted bBlockInput (the game reset it)");
    }

    // Pawn 换代（死亡/重生/切图）：旧 Pawn 的还原值不再适用，按新 Pawn 重写。
    UObject* pawn = ReadObjectProperty(g_pc, "Pawn");
    if (IsOk(pawn) && pawn != g_pawn) {
        if (g_blockWrotePawn && IsOk(g_pawn)) WriteBool(g_pawn, "bBlockInput", g_blockOrigPawn, nullptr);
        g_blockOrigPawn = false;
        g_blockWrotePawn = WriteBool(pawn, "bBlockInput", true, &g_blockOrigPawn);
        g_pawn = pawn;
    }

    // 轴屏蔽没有可读回的等效属性，直接重写（5Hz × 两次 ProcessEvent，可忽略）。
    if (g_ignoreWrote) {
        InvokeBoolSetter(g_pc, "SetIgnoreMoveInput", "bNewMoveInput", true);
        InvokeBoolSetter(g_pc, "SetIgnoreLookInput", "bNewLookInput", true);
    }
    if (g_cursorWrote) WriteBool(g_pc, "bShowMouseCursor", true, nullptr);
}

void Unfreeze() {
    if (!g_frozen) return;
    if (IsOk(g_pc)) {
        if (g_blockWrotePc) WriteBool(g_pc, "bBlockInput", g_blockOrigPc, nullptr);
        UObject* pawn = ReadObjectProperty(g_pc, "Pawn");
        if (g_blockWrotePawn && IsOk(pawn)) WriteBool(pawn, "bBlockInput", g_blockOrigPawn, nullptr);
        if (g_ignoreWrote) {
            InvokeBoolSetter(g_pc, "SetIgnoreMoveInput", "bNewMoveInput", false);
            InvokeBoolSetter(g_pc, "SetIgnoreLookInput", "bNewLookInput", false);
        }
        if (g_cursorWrote) WriteBool(g_pc, "bShowMouseCursor", g_cursorOrig, nullptr);
    }
    g_blockWrotePc = g_blockWrotePawn = g_cursorWrote = g_ignoreWrote = false;
    g_pawn = nullptr;
    g_frozen = false;
    Log("[engine] menu closed - input restored");
}

// ------------------------------------------------------------ 游戏线程入口
void OnEngineTick() {
    if (g_shutdown) return;
    const int req = g_request.exchange(0, std::memory_order_acq_rel);
    if (req > 0) {
        Freeze();
    } else if (req < 0) {
        Unfreeze();
    } else if (req == 2) {
        EnsureFrozen();
    }
    // 兜底轮询：200ms 读回一次，游戏重置就立刻补回来（焦点回归另有即时触发）。
    if (g_frozen && GetTickCount64() - g_lastEnsureMs >= 200) EnsureFrozen();
}

bool Init(const char** why) {
    if (GetModuleHandleW(L"UE4SS.dll") == nullptr) {
        if (why) *why = "UE4SS.dll is not loaded in this process";
        return false;
    }
    Hook::FCallbackOptions options{};
    options.OwnerModName = L"PBZ-Framework Menu";
    options.HookName = L"EngineTickPre";
    g_tickId = Hook::RegisterEngineTickPreCallback(
        [](Hook::TCallbackIterationData<void>&, UEngine*, float, bool) { OnEngineTick(); }, options);
    if (g_tickId == Hook::ERROR_ID) {
        if (why) *why = "UE4SS EngineTickPre callback registration failed";
        return false;
    }
    Log("[engine] EngineTickPre hook registered - freeze runs on the game thread");
    if (why) *why = "ready";
    return true;
}

void Shutdown() {
    g_shutdown = true;
    // 先解冻再注销：回调一旦注销，冻结就没人恢复了。
    g_request.store(-1, std::memory_order_release);
    Unfreeze();
    if (g_tickId != Hook::ERROR_ID) {
        // 绝不能在回调内部注销（UE4SS 会死锁）；这里在模块卸载线程执行。
        Hook::UnregisterCallback(g_tickId);
        g_tickId = Hook::ERROR_ID;
    }
    g_pc = nullptr;
}

void OnMenuOpen(bool open) {
    g_request.store(open ? 1 : -1, std::memory_order_release);
}

// 焦点回归：游戏可能刚把输入栈重建过，立刻在下一次 tick 补一次冻结。
void OnReassert() {
    if (!g_frozen) return;
    g_request.store(2, std::memory_order_release);
}

const EngineBackend kBackend{ "ue4ss", &Init, &Shutdown, &OnMenuOpen, &OnReassert };

} // namespace

const EngineBackend* EngineBackendUe4ss() {
    return &kBackend;
}

} // namespace pbzfm

#endif   // PBZFM_ENGINE_PORT_UE4SS
