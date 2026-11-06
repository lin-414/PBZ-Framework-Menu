# 方案 C 实机测试说明

本文件会随测试包一起放进 `PBZFM/`，也是仓库里的正式测试清单。

## 两个包的区别

| 包 | 输入后端 | 用途 |
| --- | --- | --- |
| `PBZ-Framework-Menu_0.6.0-engine.zip` | 引擎内冻结（方案 C）：不装任何全局钩子、不改 Raw Input | 本次要验证的包 |
| `PBZ-Framework-Menu_0.6.0-os.zip` | 旧路径：全局 LL 钩子 + Raw Input 改指向 | 同代码基线，用来 A/B 对照"录屏热键到底是不是被钩子吃掉" |

两个包打的是同一份源码，唯一差别是 DLL 里的输入后端和包里 `PBZFM.ini` 的
`input_mode`（engine 包写死 `engine`，os 包写死 `os`，这样日志不会因为自动回退而含糊）。

## 安装

解压到 `<游戏目录>\platform\steam\ue4ss\Mods\PBZFM\`，最终应当是：

```
PBZFM/
├─ enabled.txt
├─ PBZFM.ini
├─ dlls/PBZFM.dll
├─ languages/*.json
├─ sdk/pbzfm.h
├─ TESTING_ENGINE_INPUT.md      （即本文件）
└─ LICENSE / THIRD_PARTY_NOTICES.md
```

> engine 包依赖 UE4SS 3.0.x 的导出符号（与 KintaroModding fork 同一批）；如果
> UE4SS 版本不匹配，`PBZFM.dll` 会**加载失败**（游戏无菜单、`PBZFM.log` 里没有新内容）。
> os 包不依赖 UE4SS 的任何导出，可以用它来区分"是引擎包的问题"还是"环境问题"。

## 测试步骤（按顺序，每步都看日志）

日志：`文档\PBZ\Logs\PBZFM.log`

**1. 启动游戏，确认后端**

```
[engine] engine/ue4ss (input_mode=engine, no OS hooks, no raw-input hijack)
[input] backend: engine - global hooks are off, system hotkeys/recording unaffected
[engine] EngineTickPre hook registered - freeze runs on the game thread
```

关键：**不应**出现 `[input] hooks installed`。若出现
`WARNING: input_mode=engine but the engine port is unavailable`，说明引擎后端没起来，
这局不算测到方案 C，把日志发我。

**2. 菜单开着时按录屏热键**（这是本方案的核心验收点）

F8 开菜单 → 按你录屏软件的键（Win+Alt+R / Alt+F9 / F9 / 自定义）。
期望：**录屏软件有反应**（开始/停止录制、或弹出浮层）。

**3. 菜单开着时按 WASD / 移动鼠标**

期望：角色不动、镜头不转（`[engine] menu open - frozen (bBlockInput=1 ignore=1 cursor=1)`）。

**4. 菜单交互**

期望：鼠标可见且能点、能拖面板、能改设置、Esc 或 F8 能关。

**5. 关菜单后继续玩**

期望：`[engine] menu closed - input restored` 之后键鼠与镜头立即恢复。

**6. 反复开关 20 次 + 进一次过场/切图**

期望：不出现"关掉菜单后输入还被冻住"。重点看每次关闭都有 `input restored`。

**7. 对照实验（用 os 包）**

同一个场景（菜单开着按录屏热键）应该**没反应**。这一步是证明"问题确实来自钩子吞键"。

## 出问题怎么办

- 任何一步不对：先看日志有没有 `[engine] ... failed`；把 `PBZFM.log` 发我。
- 需要立刻恢复正常游玩：把 `PBZFM.ini` 的 `input_mode` 改成 `os`（或换 os 包），
  **不用重装游戏、不用换其他文件**。
- 完全卸载：删掉 `Mods\PBZFM` 目录。

## 已知边界（不是 bug）

- 引擎模式下呼出键依赖游戏窗口在前台（覆盖窗是 `WS_EX_NOACTIVATE`，所以菜单打开时
  游戏仍是前台，这一点成立）。
- 菜单开着时鼠标指针由系统绘制（引擎 `bShowMouseCursor`），不再有"自绘光标"，
  因此**录屏里能看到指针**（旧版菜单期间是没有指针的）。
- `Win+G` 这类组合键：engine 包不受影响（没有任何钩子）；os 包仍然会被吞，属旧版已知问题。
