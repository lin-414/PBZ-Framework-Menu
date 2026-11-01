# PBZ-Framework Menu — 《影之刃零》模组菜单框架

类似 Skyrim 的 [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)
的《影之刃零》（Phantom Blade Zero, UE 5.7）模组菜单框架。目标：给
[PBZPhotoMode](https://github.com/lin-414/PBZPhotoMode) 这类 UE4SS C++ DLL 模组提供
**快捷键呼出的游戏内设置界面**、多语言、以及可由框架统一编辑的模组配置（含键位）。

当前状态：**0.3.2**。0.2.0 的输入全吞 + 自绘光标方案已实机验证（2026-11-01：游戏内
F8 菜单打开期间镜头冻结、菜单光标 1:1 跟随、系统光标隐藏，关闭后游戏输入即恢复正常）。
手柄适配（XInput：长按选择键呼出/摇杆虚拟光标/XInputEnable 冻结）已开发完成并经
host 确定性回归，**应用户要求暂时搁置、未进正式包**——源码保留在 src/gamepad.*
（未编译，gamepad.h 头部有完整恢复步骤）。

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
tools\pack.ps1           :: 出 dist\PBZ-Framework-Menu_0.3.2.zip（UE4SS 模组包）
```

**部署约定**：本仓库的自动化只到"构建 + 打包 dist zip"为止——**不向游戏目录部署或复制
任何文件**。实机安装与测试由用户用模组管理器自行完成，测试结论反馈后录入上方"已验证"表。

## 游戏内安装（用户用模组管理器自行完成，自动化不碰游戏目录）

1. 安装 [UE4SS for PBZ](https://www.nexusmods.com/phantombladezero/mods/5)（KintaroModding fork）。
2. 用模组管理器（或手动解压）把 `dist/PBZ-Framework-Menu_0.3.2.zip` 装到
   `<游戏目录>\platform\steam\ue4ss\Mods\`，形成
   `ue4ss\Mods\PBZFM\{dlls\PBZFM.dll, enabled.txt, PBZFM.ini, languages\}`。
3. 启动游戏按 F8；验证清单见包内 README.txt。排障先看 `Documents\PBZ\Logs\PBZFM.log`
   （系统的“文档”目录，自动创建；该位置不可写时回落到 `ue4ss\Mods\PBZFM\PBZFM.log`）。

## 目录

```
src/            PBZFM.dll 源码（pbzfm 入口 / overlay DComp 渲染 / input 钩子 /
                gamepad XInput 手柄 / game_window 定位与子类化 / config / i18n / menu / theme）
host/           测试宿主（模拟 UnrealWindow + 脚本输入 + 截图）
assets/         随包分发文件（PBZFM.ini、languages/*.json、README.txt）
tools/          build.bat / pack.ps1
third_party/    ImGui 1.92.1（vendored）、stb_image_write.h
docs/           自测截图
```

## 下一阶段（按优先级）

1. **实机验证**（用户用模组管理器安装 dist zip 后自行测试并反馈结论）：游戏内 F8、
   FSR3 帧生成开着跑、真独占全屏/多显示器边角；菜单打开时镜头不跟随鼠标移动、
   光标状态开/关往返不破坏。
2. **注册 API**（C ABI 导出 + SDK 头，对齐 SKSEMenuFramework.h 形态）：
   `SetSection / AddSectionItem(渲染回调) / AddWindow / AddHudElement / AddInputEvent /
   AddConfigSchema(设置项描述) / 翻译表注册`；客户端模组 `GetModuleHandleW(L"PBZFM.dll")`
   + `GetProcAddress` 接入（同进程）。
3. **设置 schema**：toggle/slider/keybind/enum/combo 通用控件 + 按 mod 写回 config.ini + 变更回调。
4. 手柄导航：开发完成（0.3.1 host 回归全绿）但**应用户要求暂时搁置、未进正式包**；
   源码保留在 src/gamepad.*（未编译，恢复步骤见 gamepad.h 头注）。剩余：游戏暂停
   （复用 PBZPhotoMode 已验证的暂停栈）、主题 JSON。
5. 把 PBZPhotoMode 的 ImGui 诊断面板接成第一个客户端（其 MenuView/MenuActions 快照桥直接复用）。
