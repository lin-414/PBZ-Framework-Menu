# 方案 C：引擎内输入（Engine Port）

> 目标：把「冻结游戏」从 **OS 层劫持系统输入**（全局 LL 钩子 + Raw Input 改指向）
> 换成 **引擎层冻结本进程输入**（`bBlockInput` / `SetIgnore*Input`）。
> 这样系统输入流一个字节都不被拦截，录屏 / 截图 / 游戏栏 / Steam 的全局热键在
> 菜单打开时照常有效。

## 1. 职责边界（先看这条，避免误解）

| 子系统 | 谁负责 | 说明 |
| --- | --- | --- |
| 冻结游戏输入（键鼠动作 / 移动轴 / 视角轴） | **引擎后端** | 只在游戏线程调用，天然只影响本进程 |
| 菜单的键鼠事件采集 | **窗口消息**（覆盖窗 + 游戏窗口子类） | 本作是 Enhanced Input，`PlayerController` 上没有可钩的 `InputKey`/`InputAxis`（PBZPhotoMode 实机结论）；引擎侧逐键轮询要每帧上百次 `ProcessEvent`，不划算也不可靠 |
| 光标 | **引擎后端**（`bShowMouseCursor`） | ImGui 不再自绘光标，避免双光标 |
| 全局热键 / 录屏 | **不再被碰** | 模块不再安装任何 `WH_KEYBOARD_LL` / `WH_MOUSE_LL`，不再改 Raw Input |

也就是说：方案 C 负责「冻结」，方案 A 的窗口消息通道负责「取输入」。两者合起来
才是完整的零 OS 侵入方案。

## 2. 结构

```
pbzfm.cpp  InitThreadProc
  └─ EnginePortInit(input_mode)        engine_port.cpp（纯状态机，可单测）
       ├─ 探测后端 EngineBackendGet()  ← 只有 PBZFM_ENGINE_PORT=ON 才有实现
       ├─ 不可用 → 返回 false → 调用方装 OS 钩子（0.5.x 行为）
       └─ 可用   → 返回 true  → 调用方不装任何全局钩子

overlay.cpp SetVisible(true/false)
  └─ EnginePortSetMenuOpen(v)          → engine_port_ue4ss.cpp：只写一个原子请求

游戏线程（UE4SS EngineTickPre 回调）
  └─ OnEngineTick → Freeze()/Unfreeze()
       ├─ FindAllOf("PlayerController") 挑 Player+Pawn 双属性的那个
       ├─ bBlockInput = true（PC + Pawn，写前存旧值，恢复按位还原）
       ├─ SetIgnoreMoveInput / SetIgnoreLookInput(true/false)
       └─ bShowMouseCursor = true/false
```

关键纪律（照抄 PBZPhotoMode 的实机铁律）：

1. **渲染线程永不碰 UObject**；`menuOpen` 只写 `std::atomic<int>`。
2. **`__try` 只出现在不含带析构局部对象的叶子函数里**（否则 `/EHsc` 报 C2712）。
3. **按位恢复**：只还原自己写成功过的属性，PC/Pawn 换代后不写回旧对象。
4. **失败即降级**：找不到 PlayerController 或反射调用失败 → 记一次日志后不再重试，
   菜单本身仍然可用（只是游戏不冻结）。
5. **fmt 必须在 Unreal 头之前 include**：Unreal 的 `WindowsPlatformCompilerSetup.hpp`
   把 4582/4583 提升为 error，而 fmt 的 `get_locale` 正好触发。

## 3. 构建

默认（发布包）：

```bat
build_all.bat          :: 零 UE4SS 依赖，输入走 OS 钩子
```

引擎输入版（需要本机已有 UE4SS 源码头 + 预编译 import lib；不需要 Rust、不需要
`add_subdirectory` 整个 UE4SS）：

```bat
build_engine.bat
:: 等价于
cmake -S . -B build_engine -G Ninja -DCMAKE_BUILD_TYPE=Release ^
      -DPBZFM_ENGINE_PORT=ON ^
      -DPBZFM_UE4SS_DIR=D:/Documents/DSH/PBZ/_ref/ue4ss-src ^
      -DPBZFM_UE4SS_LIBDIR=D:/Documents/DSH/PBZ/PBZPhotoModeCpp/build-dll/Game__Shipping__Win64/lib ^
      -DPBZFM_UE4SS_GENERATED_DIR=D:/Documents/DSH/PBZ/PBZPhotoModeCpp/build-dll/ue4ss/UE4SS/generated
```

要显式从源码构建 UE4SS（重：C++23 + Rust/patternsleuth），加
`-DPBZFM_UE4SS_SOURCE_BUILD=ON`。

> 引擎版必须用 **动态 CRT**（`/MD`）：UE4SS 的 tick 回调按值传 `std::function`，
> 与 `/MT` 混用会在跨模块释放时崩。CMake 在 `PBZFM_ENGINE_PORT=ON` 时自动切换。

## 4. 运行期

`PBZFM.ini`：

```ini
[framework]
input_mode = auto     ; auto | engine | os
```

日志（实机 init 阶段）：

```
[engine] engine/ue4ss (input_mode=auto, no OS hooks, no raw-input hijack)
[input] backend: engine - global hooks are off, system hotkeys/recording unaffected
[engine] EngineTickPre hook registered - freeze runs on the game thread
[engine] menu open - frozen (bBlockInput=1 ignore=1 cursor=1)
[engine] menu closed - input restored
```

回退路径（没装引擎版 DLL / 没有 UE4SS / 显式 `input_mode = os`）：

```
[engine] os (input_mode=auto, this build has no engine backend ...)
[input] backend: os (global LL hooks + raw-input re-point)
```

## 5. 已验证（本机，2026-11-05）

| 项 | 结果 |
| --- | --- |
| `PBZFM_ENGINE_PORT=OFF` 构建 | ✅ 通过；导入表无 UE4SS（USER32/GDI32/SHELL32/KERNEL32/ole32/IMM32/D3DCOMPILER_47） |
| `PBZFM_ENGINE_PORT=ON` 构建（预编译 import lib 路线） | ✅ 通过；导入表含 `UE4SS.dll` + 动态 CRT（MSVCP140/VCRUNTIME140/api-ms-win-crt-*） |
| `ctest`（两套构建） | ✅ i18n_test + framework_test 全绿 |
| 新增单测：后端策略 / `input_mode` 读写 | ✅ `C input backend policy`、`C input_mode config` |
| 引擎后端源码编译（真实 UE4SS 头 + UE4SS.lib） | ✅ 编译+链接通过 |

## 6. 待实机验证（必须做，不含糊）

| # | 步骤 | 期望 |
| --- | --- | --- |
| 1 | 装引擎版 DLL + UE4SS，进游戏看日志 | `backend: engine`，无 `hooks installed` |
| 2 | 菜单开着按 Win+Alt+R / Alt+F9 / F9 | 录屏软件**有反应**（这是本方案的核心验收点） |
| 3 | 菜单开着按 WASD / 鼠标 | 角色不动、镜头不转 |
| 4 | 菜单可点、可拖、Esc 可关 | 同旧版 |
| 5 | 关菜单后继续玩 | 键鼠/镜头立即恢复 |
| 6 | 反复开关 20 次 + 切图/过场 | 无崩溃、无永久冻结（重点看 `bBlockInput` 是否按位还原） |
| 7 | `input_mode = os` 强制回退 | 行为与 0.5.2 完全一致 |

第 6 步是风险点：`bBlockInput` 是直接写属性，如果游戏在过场里自己改过它，我们的
"存旧值 → 按位还原"可能写回一个过期值。实机若发现输入被永久冻结，先看
`[engine] menu closed - input restored` 是否出现，再用 `input_mode = os` 兜底。

## 7. 风险与回滚

- **风险 1**：`bShowMouseCursor` / `GetMousePosition` 在本作零引用（PBZPhotoMode 未验证过）。
  本方案只用 `bShowMouseCursor` 写值，即使失效也只是"菜单里看不到系统光标"，不影响冻结。
- **风险 2**：引擎版 DLL 依赖 UE4SS 的导出符号；UE4SS 与游戏版本不匹配时 DLL 直接加载失败。
  → 引擎版与默认版分开出包，默认包保持零依赖。
- **回滚**：`input_mode = os` 即回到旧路径，不需要换 DLL。
