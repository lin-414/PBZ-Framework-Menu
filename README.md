# PBZ-Framework Menu — 《影之刃零》模组菜单框架

类似 Skyrim 的 [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)
的《影之刃零》（Phantom Blade Zero, UE 5.7）模组菜单框架。目标：给
[PBZPhotoMode](https://github.com/lin-414/PBZPhotoMode) 这类 UE4SS C++ DLL 模组提供
**快捷键呼出的游戏内设置界面**、多语言、以及可由框架统一编辑的模组配置（含键位）。
模组如何注册自己的分区、设置页与热键，见下方《模组作者接入指南》。

当前状态：**0.3.3**。0.2.0 的输入全吞 + 自绘光标方案已实机验证（2026-11-01：游戏内
F8 菜单打开期间镜头冻结、菜单光标 1:1 跟随、系统光标隐藏，关闭后游戏输入即恢复正常）。
0.3.3 修复菜单打开期间 Win 切到开始菜单暴露的两个实机问题（开始菜单光标冻死、回到游戏
后镜头跟随鼠标）：吞输入增加前台判断 + 焦点回归时即时重新抢占 Raw Input/钩子链/ClipCursor。
**待实机复核**。
手柄适配（XInput：长按选择键呼出/摇杆虚拟光标/XInputEnable 冻结）已开发完成并经
host 确定性回归，**应用户要求暂时搁置、未进正式包**——源码保留在 src/gamepad.*
（未编译，gamepad.h 头部有完整恢复步骤）。
模组接入的注册 API（C ABI 导出 + SDK 头 + 声明式设置 schema + Widget API 自绘页 +
游戏内热键）已实现：零依赖接入头 `sdk/pbzfm.h`（随 dist zip 分发于 `PBZFM/sdk/`），
参考实现 `host/demo_mod.cpp`（独立 DLL、无 ImGui、无 UE4SS 依赖，host 文件断言回归）。
接入文档见下方《模组作者接入指南》。

![菜单截图](docs/prototype_menu_open.png)

## 架构：为什么是 DirectComposition 独立覆盖窗

PBZPhotoMode 的实机取证（其 PROGRESS.md"六连判死"）证明了两条常规路线在本作不可行：

1. **引擎内 UI**（IoStore pak / UMG / WBP / puerts）：定制引擎序列化器拒收、新包名进不了
   名字→包 ID 解析、Shipping 未注册 puerts 模块——全部死路；
2. **DX 交换链 Present 钩子 + ImGui**：与 FSR3 帧生成（amd_fidelityfx_framegeneration_dx12）
   + NVIDIA Streamline 的交换链包装冲突，触发设备移除 → 游戏 RHI fatal。

唯一剩余路线（PBZPhotoMode 终审结论）：**OS 合成器层的独立覆盖窗**。
本原型即此路线的最小实现：

- 顶层窗口 `WS_EX_NOREDIRECTIONBITMAP | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST`，
  跟踪游戏 UnrealWindow 的位置/尺寸；
- `CreateSwapChainForComposition`（FLIP_SEQUENTIAL + PREMULTIPLIED alpha）+ 私有 D3D11
  设备 + ImGui 1.92.1，**完全不碰游戏的交换链与 D3D 对象**；
- d3d11/dxgi/dcomp 全部运行时 `LoadLibrary`（导入表只有系统基础 DLL），初始化推迟到游戏窗口
  出现之后（PBZPhotoMode 1.1.0 的加载期撞 RHI 教训）；
- 输入：低级键盘/鼠标钩子。菜单关闭只截呼出键；菜单打开吞全部键鼠喂给 ImGui
  （鼠标移动同样吞掉——游戏经 Raw Input/鼠标捕获响应镜头，与光标下的窗口无关，
  放行会导致游戏画面跟随鼠标；钩子把绝对位置直接喂给 overlay，菜单光标由 ImGui
  自绘；游戏自己 SendInput 的合成移动只吞不喂），Esc 或呼出键关闭；
  吞全的前提是游戏窗口在前台——Win（开始菜单）/Alt-Tab/点击其他窗口切走后钩子全部放行，
  桌面上系统光标与键盘照常（实机 2026-11-01：否则开始菜单光标冻死），呼出键与 Esc 仍生效，
  回到游戏后按事件增量重新基准轨迹；
  LL 钩子链"后装者先执行"——游戏输入栈/反作弊可能在模组加载后才装自己的低级钩子并
  先行吃掉移动（实机 2026-11-01 症状：双光标重合冻结、镜头仍跟随），因此菜单打开时
  本框架钩子会卸载重装插队到链首，关闭后保持链首直通；移动全部吞掉后 OS 光标不前进，
  每个事件的 pt=冻结位置+本事件增量（实机 raw≈hookMoves 证明本系统 Raw Input 不受
  LL 钩子阻断），故按"事件增量相对真实位置"自累积光标轨迹喂 ImGui（瞬移自归零，
  禁止每帧干预游戏的 ClipCursor/ShowCursor——实测会卡死游戏输入状态机）；系统光标
  悬停在覆盖窗上，其可见性归覆盖窗线程队列所有，打开时在渲染线程直接排到隐藏。
  另两条实机铁律：Raw Input 注册是进程级、每设备一条，RegisterRawInputDevices 会
  顶掉游戏同名设备的注册（本框架借此在菜单期间断掉相机增量——打开时改指向覆盖窗、
  关闭时按保存的原表原样恢复）；RIDEV_REMOVE 是进程级摘除，会连带拆掉游戏自己的
  Raw Input 注册（相机失效直到重启，2026-11-01 实测），任何情况下不得在关闭路径调用；
  对游戏队列做每帧 ShowCursor/ClipCursor 干预同样会卡死其输入状态机，均已在终版移除。
  第三条实机铁律：游戏窗口重新激活会重注册 Raw Input、重设 ClipCursor，其输入栈还可能
  重装 LL 钩子插回链首——菜单打开期间焦点回归即触发重新抢占（钩子插队 + 解除裁剪 + 按
  当前注册表重新改指向），另以 4Hz 轮询注册归属兜底（Steam Input 这类会随时重注册）；
  重新改指向前必须先核对注册仍指向覆盖窗，否则会把本框架自己的注册存成恢复表，关闭后
  相机失效直到重启（0.3.3）。
  已注册的模组热键在菜单打开时保持生效（分发后吞键、不进菜单控件；未注册的按键——
  如 PhotoMode 的截图键——仍被吞，需模组侧经 RegistryAddHotkey 注册才会分发）；
- 光标显示/隐藏通过子类化游戏 WndProc 投递到窗口线程执行（ShowCursor 是线程相关的）；
- 框架设置页自带"截取屏幕"按钮：BitBlt 合成后的桌面区域（游戏窗口范围，**含菜单
  界面本身**），PNG 存到 ModRoot\Screenshots\——N卡/PixPin 这类全局截图工具的热键
  在菜单打开期间被输入隔离拦截且交互式选区工具拿不到鼠标，由框架自带截图兜底；
- 配置：PBZFM.ini（容错解析，菜单保存写回）；多语言：languages/*.json（扁平 JSON，
  热切换，内嵌兜底，内置 zh-CN / en-US / ja-JP / ko-KR）；客户端模组的分区名/页签/
  设置项 label 以文本为 key 参与查表（查不到原样显示）；CJK 字体：按界面语言加载系统
  字体（中文/日文用微软雅黑，韩文 UI 优先 Malgun Gothic，ImGui 1.92 动态字体图集）；
  界面主题：玄墨鎏金（src/theme，取自游戏 HUD 的配色语言——玄墨底、银色发丝线、
  鎏金高亮、朱砂点缀，全直角 + 1px 细边框，标题栏金线/银括角/朱砂印章装饰，
  键位捕获态为朱砂；装饰经前景绘制列表绘制，规避 1.92 窗口列表的标题栏裁剪）。

## 已验证（模拟游戏窗口，host 自测）

| 项 | 结果 |
| --- | --- |
| 覆盖窗 + DComp 合成 + ImGui 渲染（含中文、逐像素透明） | ✅ |
| F8 快捷键呼出/关闭（低级键盘钩子） | ✅ |
| 菜单打开时鼠标点击命中 ImGui 控件（计数 0→1） | ✅ |
| 配置保存/加载（PBZFM.ini 往返） | ✅ |
| 语言表加载与切换（zh-CN/en-US/ja-JP/ko-KR 内嵌兜底 + 磁盘覆盖回归，24 条/语言） | ✅ |
| 微软雅黑 CJK 字体加载 | ✅ |
| 玄墨鎏金主题（HUD 配色/直角发丝线/标题栏装饰，host 截图比对） | ✅ 2026-11-01 |
| 框架设置入口移出侧边栏（模组列表不再混入框架项），面板左下角"设置"按钮直达框架设置页 | ✅ 2026-11-01 |
| 导入表无 d3d11/dxgi/dcomp（运行时加载） | ✅ |
| 干净卸载（host FreeLibrary 全链退出） | ✅ |
| **实机**：UE4SS 生命周期接入（start_mod/uninstall_mod 空实现） | ✅ 2026-11-01 |
| **实机**：游戏内 F8 开关菜单、2560x1440 渲染、FSR3 帧生成环境稳定 | ✅ 2026-11-01 |
| **实机**：与 PBZPhotoMode 共存（F2 摄影模式 + F8 框架菜单同局） | ✅ 2026-11-01 |
| **实机**：光标管理——游戏进行中开/关、游戏菜单内开/关两种路径均不破坏游戏光标状态 | ✅ 2026-11-01 |
| 菜单打开时鼠标移动输入全吞（游戏镜头不跟随移动）+ ImGui 自绘菜单光标 | ✅ 2026-11-01 |
| **实机**：菜单打开期间镜头冻结、菜单光标 1:1 跟随、系统光标隐藏；关闭后相机与游戏菜单光标即恢复 | ✅ 2026-11-01 |
| **实机**：经框架界面改绑 PBZPhotoMode 键位（key_toggle 捕获）并正常保存 | ✅ 2026-11-01 |
| **实机**：界面拖动移动/调整大小保留（会话内） | ✅ 2026-11-01 |
| 手柄适配（长按选择键呼出/A 点击/摇杆/XInputEnable 冻结往返）host 回归全绿 | ✅ 2026-11-01（0.3.1 已搁置，未进包） |

自测方式：`host/pbzfm_host.exe` 注册一个名为 `UnrealWindow` 的窗口（框架寻找的目标类）、
`LoadLibrary(PBZFM.dll)`（与 UE4SS 加载方式一致），脚本化 SendInput：F8 → 移动 → 点击 →
逐帧截图到 `build/shots/`。点击目标由 DLL 导出的 `PBZFM_GetDemoButton` 实时查询。
加 `--synthetic` 则改走 DLL 导出的合成输入通道（PBZFM_TestInjectMove/Button/ToggleMenu/
TestDispatchHotkey）：不经 OS 输入流，可在有人正在使用桌面时确定性回归。真实注入路径
依赖无人干扰的输入桌面——UAC/锁屏等安全桌面激活期间 LL 钩子收不到任何输入，属系统行为。

## 构建

需要 VS 2022 BuildTools（C++ 工具集 + 自带 CMake/Ninja）。不依赖 RE-UE4SS 源码、
不需要 UEPseudo/Rust——原型 DLL 对 UE4SS 是零依赖的普通 DLL。

```bat
build_all.bat            :: 构建（cmake + ninja），产物 build\PBZFM.dll + build\pbzfm_host.exe
tools\pack.ps1           :: 出 dist\PBZ-Framework-Menu_0.3.3.zip（UE4SS 模组包）
```

**部署约定**：本仓库的自动化只到"构建 + 打包 dist zip"为止——**不向游戏目录部署或复制
任何文件**。实机安装与测试由用户用模组管理器自行完成，测试结论反馈后录入上方"已验证"表。

## 游戏内安装（用户用模组管理器自行完成，自动化不碰游戏目录）

1. 安装 [UE4SS for PBZ](https://www.nexusmods.com/phantombladezero/mods/5)（KintaroModding fork）。
2. 用模组管理器（或手动解压）把 `dist/PBZ-Framework-Menu_0.3.3.zip` 装到
   `<游戏目录>\platform\steam\ue4ss\Mods\`，形成
   `ue4ss\Mods\PBZFM\{dlls\PBZFM.dll, enabled.txt, PBZFM.ini, languages\}`。
3. 启动游戏按 F8；验证清单见包内 README.txt。排障先看 `Documents\PBZ\Logs\PBZFM.log`
   （系统的“文档”目录，自动创建；该位置不可写时回落到 `ue4ss\Mods\PBZFM\PBZFM.log`）。

## 模组作者接入指南（C API / SDK）

任何加载进游戏进程的 DLL（实践中就是 UE4SS C++ DLL 模组，PBZPhotoMode 形态）都可以
向菜单注册自己的侧边栏分区、设置页、自绘页与游戏内热键。接入**零构建依赖**：只
include [sdk/pbzfm.h](sdk/pbzfm.h)（dist zip 内路径 `PBZFM/sdk/pbzfm.h`），唯一
外部头是 `windows.h`；所有函数运行时经 `GetProcAddress` 解析，不链接导入库、
不需要自带 ImGui。

### 工作原理与前提

- 框架本体是 UE4SS 模组 `ue4ss\Mods\PBZFM\dlls\PBZFM.dll`。你的模组调用内联的
  `PBZFM_LoadApi(&api)`：内部 `GetModuleHandleW(L"PBZFM.dll")` 找到框架、解析全部
  函数指针，并校验运行时 ABI 版本等于编译时的 `PBZFM_ABI_VERSION`（当前 1），任一
  环节失败返回 0。
- UE4SS 按目录名字母序加载模组，PBZFM 可能晚于你的 DLL 出现：**不要在 DllMain 里
  注册**。在 DllMain 里只起一个自己的线程，轮询 `PBZFM_LoadApi` 直到成功（参考
  实现轮询 200 × 100ms）。
- 框架未安装/未启用是常态而非错误：`PBZFM_LoadApi` 返回 0 时跳过注册路径，你的
  模组必须能独立运行，菜单页只是增强。
- 侧边栏分区名与 schema 的 store 名是**全进程共享的命名空间**：store 名用全局唯一
  标识（如 `作者名.模组名`），否则同名 store 会互相覆盖 `data/<store>.json`。

### 最小接入

```cpp
#include "pbzfm.h"        // 即 sdk/pbzfm.h，建议随你的源码 vendored

static PBZFM_Api    api;      // 全部函数指针
static PBZFM_Config cfg;      // schema 设置页句柄（opaque）

static void OnChange(const char* key, void*) {      // 框架渲染线程
    if (strcmp(key, "enable_fx") == 0)
        ApplyFx(api.GetBool(cfg, "enable_fx"));     // 值由框架持有，回调里读回
}

static DWORD WINAPI Init(LPVOID) {
    for (int i = 0; i < 200; ++i) {                 // 最多等 20s
        if (PBZFM_LoadApi(&api)) break;
        Sleep(100);
    }
    if (!api.GetAbiVersion) return 0;               // 框架不在：照常运行，无菜单页

    api.SetSection("我的模组");                      // 创建/复用侧边栏分区
    cfg = api.ConfigCreate("author.mymod", "设置");  // store 全局唯一 + 页签名
    api.ConfigAddBool(cfg, "enable_fx", "启用特效", 1);
    api.ConfigOnChange(cfg, OnChange, nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        CreateThread(nullptr, 0, Init, nullptr, 0, nullptr);
    }
    return TRUE;
}
```

完整可编译参考：[host/demo_mod.cpp](host/demo_mod.cpp)——schema 页 + 自定义页 +
游戏热键三件套，共 116 行、无任何框架外依赖。

### 线程契约

| 场景 | 运行线程 | 注意 |
| --- | --- | --- |
| SetSection / ConfigCreate / ConfigAdd* / AddHotkey 注册 | 任意（自己的线程） | 绝不在 DllMain；框架未加载就轮询 |
| 自定义页渲染回调（AddSectionItem 的 fn） | 框架渲染线程 | 仅菜单打开且你的页签选中时每帧执行；保持快速、绝不阻塞 |
| 设置变更回调（ConfigOnChange 的 fn） | 框架渲染线程 | 每次提交编辑触发；值已更新，用 Get* 读回 |
| 游戏热键回调（AddHotkey 的 fn） | 框架输入钩子线程 | 需要游戏线程的数据自行 marshal |

### 两类页面

菜单左侧是分区（section），每个分区含若干页签（page）。`SetSection("名字")` 创建或
复用同名分区并设为**调用线程的当前分区**，随后的 `ConfigCreate` 与
`AddSectionItem` 都加进该分区；同一模组可建多个分区（demo_mod 就建了两个）。页签
两类：

1. **Schema 设置页**（声明式，推荐）：你声明控件，框架负责渲染、编辑交互、持久化
   与变更回调，你不写一行 UI 代码；
2. **自定义页**：`AddSectionItem("页签名", 回调, userdata)`，菜单打开且该页选中时
   每帧调用你的回调，用 Widget API 自绘。

### Schema 设置页：控件与提交语义

```cpp
api.ConfigAddGroup(cfg, "通用");                                   // 分组标题（纯布局）
api.ConfigAddBool (cfg, "enable_fx", "启用特效", 1);               // 复选框，值 0/1
api.ConfigAddInt  (cfg, "level",     "等级", 2, 0, 4);             // 整数滑条 [min,max]
api.ConfigAddFloat(cfg, "fov",       "视野", 90.f, 15.f, 150.f, 1.f, "%.0f");
const char* presets[] = { "低", "中", "高" };
api.ConfigAddEnum (cfg, "preset",    "画质预设", 0, presets, 3);   // 下拉框，值为选项下标
api.ConfigAddKey  (cfg, "shot_key",  "截图键", VK_F9);             // 键位：Win32 VK，
                                                                   // 菜单内点击进入捕获态
```

- **提交时机**：BOOL / ENUM / KEY 改动立即触发变更回调；INT / FLOAT 在滑条松手时
  触发一次（拖动过程中不刷回调）。
- **持久化**：改动先标脏；`ConfigSave(cfg)` 立即写文件，或等菜单关闭时框架统一
  保存全部脏 store。文件位于 `ue4ss\Mods\PBZFM\data\<store>.json`，下次启动
  `ConfigCreate` 自动加载，同名 key 覆盖你声明的默认值——所以调默认值不会把玩家
  已保存的设置改回去。
- `GetBool / GetFloat / GetInt / GetEnum / GetKey` 读当前值；
  `SetBool / SetFloat / SetInt / SetEnum / SetKey` 可在你的任意线程程序化写值，
  语义与界面编辑一致（标脏 + 触发变更回调）。
- `ConfigOnChange` 可不注册：不注册时框架照样渲染与持久化，只是没有实时生效钩子。

### 自定义页与 Widget API

```cpp
static void RenderAbout(void*) {          // 框架渲染线程，菜单打开且本页选中时每帧执行
    api.Text("My Mod v1.2");
    api.Separator();
    static int clicks = 0;
    if (api.Button("点我")) ++clicks;      // 返回 1 = 本帧被点击
    api.SameLine();
    char line[64];
    sprintf_s(line, sizeof(line), "clicks: %d", clicks);  // Text 无格式化，先格式化到缓冲
    api.Text(line);
    float r[4];
    api.LastItemRect(r);                   // 最后一个控件的矩形（覆盖窗客户区像素）
}
api.AddSectionItem("关于", RenderAbout, nullptr);   // 加进当前分区
```

Widget API 仅在渲染回调内有效：`Text / TextDisabled / Separator / SameLine / Button /
Checkbox / SliderFloat / SliderInt / Combo / LastItemRect`（均为 ImGui 对应控件的薄
封装，参数直接透传）。回调卡住 = 整个菜单卡住。

### 游戏内热键

```cpp
api.AddHotkey(VK_F9, OnScreenshot, nullptr);   // 仅按下沿触发
```

- 菜单**关闭**时：按键匹配注册热键 → 触发回调，该键被吞（游戏收不到）。
- 菜单**打开**时：注册热键**保持生效**（截图、摄影模式开关这类元功能在菜单期间也
  要可用），分发后吞键、不会进菜单控件。
- 框架呼出键（默认 F8，可在框架设置页改，存 `PBZFM.ini` 的 `[framework] toggle_key`）
  在菜单关闭时**游戏也会收到**——不要把呼出键本身注册为游戏热键。
- 键位值是 Win32 虚拟键码；schema 页的 `ConfigAddKey` 项在菜单内点击后按任意键
  完成捕获，无需你写捕获逻辑。

### 高级：直接用 ImGui

`PBZFM_GetImGuiContext()` 返回框架的 `ImGuiContext*`。若你的模组**静态链接了与
框架完全相同的 ImGui 版本**（框架 vendored 1.92.1），可在渲染回调里
`ImGui::SetCurrentContext(...)` 后直接使用 ImGui 全量 API。版本不匹配 = 内存损坏：
只在 Widget API 不够用时才用，并在发布时锁定框架 README 注明的 ImGui 版本。

### 多语言

分区名、页签名、设置项 label 一律**以文本为 key** 参与框架翻译查表：当前语言表 →
en-US 表 → 查不到原样显示。因此：

- 只面向单一语言：直接传玩家可见文本即可；
- 想跟随框架的四种界面语言（zh-CN / en-US / ja-JP / ko-KR）：在框架包的
  `ue4ss\Mods\PBZFM\languages\<lang>.json`（扁平 JSON；该文件存在时整体覆盖
  内嵌表，请在现有文件上追加而非新建）加入 `"你的 label 文本": "对应翻译"`。该
  目录属于框架包、框架更新会覆盖——欢迎把条目 PR 到本仓库 `assets/languages/`，
  随框架内置。

### 分发与文件落点

- 你的模组按普通 UE4SS C++ 模组打包：`ue4ss\Mods\<你的模组>\{dlls\xxx.dll,
  enabled.txt}`，README 里声明前置依赖 PBZ-Framework-Menu。**不要**把 PBZFM.dll、
  sdk 头文件副本或任何 ImGui 运行库打进你的包。

| 文件 | 位置 |
| --- | --- |
| 框架本体 | `ue4ss\Mods\PBZFM\dlls\PBZFM.dll` |
| 框架配置（呼出键 / 语言 / UI 缩放与不透明度） | `ue4ss\Mods\PBZFM\PBZFM.ini` |
| 你的设置数据 | `ue4ss\Mods\PBZFM\data\<store>.json` |
| 翻译表 | `ue4ss\Mods\PBZFM\languages\<lang>.json` |
| 日志（排障先看） | `Documents\PBZ\Logs\PBZFM.log`（不可写时回落 `Mods\PBZFM\PBZFM.log`） |
| 框架"截取屏幕"按钮产物 | `ue4ss\Mods\PBZFM\Screenshots\` |

另有 `PBZFM_IsMenuOpen()` 供客户端感知菜单开关；`PBZFM_GetSectionCount /
GetSectionNameUtf8 / GetSectionRect / GetLastSchemaItemRect` 是 host 自动化测试用的
几何查询，常规模组不需要。

## 目录

```
src/            PBZFM.dll 源码（pbzfm 入口 / overlay DComp 渲染 / input 钩子 /
                gamepad XInput 手柄 / game_window 定位与子类化 / config / i18n / menu /
                theme / api+registry+schema 模组注册 C API）
sdk/            模组接入头文件 pbzfm.h（见《模组作者接入指南》；随包分发于 PBZFM/sdk/）
host/           测试宿主（模拟 UnrealWindow + 脚本输入 + 截图；demo_mod.cpp 为模组接入参考实现）
assets/         随包分发文件（PBZFM.ini、languages/*.json、README.txt）
tools/          build.bat / pack.ps1
third_party/    ImGui 1.92.1（vendored）、stb_image_write.h
docs/           自测截图
```

## 下一阶段（按优先级）

原计划的"注册 API"与"设置 schema"已落地为 sdk/pbzfm.h + src/api.cpp + src/schema.cpp
（接入文档见《模组作者接入指南》）；原方案里的 AddWindow / AddHudElement 未包含，
翻译不走注册 API 而是分区名/页签名/label 以文本为 key 查语言表。

1. **实机验证**（用户用模组管理器安装 dist zip 后自行测试并反馈结论）：0.3.3 的
   前台判断 + 焦点回归重新抢占待实机复核；剩余环境边角：真独占全屏/多显示器、HDR、
   Steam Overlay。
2. 手柄导航：开发完成（0.3.1 host 回归全绿）但**应用户要求暂时搁置、未进正式包**；
   源码保留在 src/gamepad.*（未编译，恢复步骤见 gamepad.h 头注）。剩余：游戏暂停
   （复用 PBZPhotoMode 已验证的暂停栈）、主题 JSON。
3. 把 PBZPhotoMode 的 ImGui 诊断面板接成第一个客户端（其 MenuView/MenuActions 快照桥直接复用）。
