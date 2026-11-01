# PBZ-Framework Menu — 《影之刃零》模组菜单框架

类似 Skyrim 的 [SKSE Menu Framework 3](https://github.com/QTR-Modding/SKSE-Menu-Framework-3)
的《影之刃零》（Phantom Blade Zero, UE 5.7）模组菜单框架。目标：给
[PBZPhotoMode](https://github.com/lin-414/PBZPhotoMode) 这类 UE4SS C++ DLL 模组提供
**快捷键呼出的游戏内设置界面**、多语言、以及可由框架统一编辑的模组配置（含键位）。

当前状态：**0.1.0 原型已在模拟游戏窗口上端到端验证**（见下方"已验证"），待实机验证。

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
  （鼠标移动放行以保证光标可见），Esc 或呼出键关闭；
- 光标显示/隐藏通过子类化游戏 WndProc 投递到窗口线程执行（ShowCursor 是线程相关的）；
- 配置：PBZFM.ini（容错解析，菜单保存写回）；多语言：languages/*.json（扁平 JSON，
  热切换，内嵌兜底）；CJK 字体：加载系统微软雅黑（ImGui 1.92 动态字体图集）。

## 已验证（模拟游戏窗口，host 自测）

| 项 | 结果 |
| --- | --- |
| 覆盖窗 + DComp 合成 + ImGui 渲染（含中文、逐像素透明） | ✅ |
| F8 快捷键呼出/关闭（低级键盘钩子） | ✅ |
| 菜单打开时鼠标点击命中 ImGui 控件（计数 0→1） | ✅ |
| 配置保存/加载（PBZFM.ini 往返） | ✅ |
| 中英语言表加载与切换（22 条/语言） | ✅ |
| 微软雅黑 CJK 字体加载 | ✅ |
| 导入表无 d3d11/dxgi/dcomp（运行时加载） | ✅ |
| 干净卸载（host FreeLibrary 全链退出） | ✅ |
| **实机**：UE4SS 生命周期接入（start_mod/uninstall_mod 空实现） | ✅ 2026-11-01 |
| **实机**：游戏内 F8 开关菜单、2560x1440 渲染、FSR3 帧生成环境稳定 | ✅ 2026-11-01 |
| **实机**：与 PBZPhotoMode 共存（F2 摄影模式 + F8 框架菜单同局） | ✅ 2026-11-01 |
| **实机**：光标管理——游戏进行中开/关、游戏菜单内开/关两种路径均不破坏游戏光标状态 | ✅ 2026-11-01 |

自测方式：`host/pbzfm_host.exe` 注册一个名为 `UnrealWindow` 的窗口（框架寻找的目标类）、
`LoadLibrary(PBZFM.dll)`（与 UE4SS 加载方式一致），脚本化 SendInput：F8 → 移动 → 点击 →
逐帧截图到 `build/shots/`。点击目标由 DLL 导出的 `PBZFM_GetDemoButton` 实时查询。

## 构建

需要 VS 2022 BuildTools（C++ 工具集 + 自带 CMake/Ninja）。不依赖 RE-UE4SS 源码、
不需要 UEPseudo/Rust——原型 DLL 对 UE4SS 是零依赖的普通 DLL。

```bat
tools\build.bat          :: 或在 cmd 里运行根目录 build_all.bat
:: 产物 build\PBZFM.dll + build\pbzfm_host.exe
tools\pack.ps1           :: 出 dist\PBZ-Framework-Menu_0.2.0.zip（UE4SS 模组包）
```

## 游戏内安装与验证

1. 安装 [UE4SS for PBZ](https://www.nexusmods.com/phantombladezero/mods/5)（KintaroModding fork）。
2. 解压 `dist/PBZFM_0.1.0-prototype.zip` 到 `<游戏目录>\platform\steam\ue4ss\Mods\`，
   形成 `ue4ss\Mods\PBZFM\{dlls\PBZFM.dll, enabled.txt, PBZFM.ini, languages\}`。
3. 启动游戏按 F8；验证清单见包内 README.txt。排障先看 `ue4ss\Mods\PBZFM\PBZFM.log`。

## 目录

```
src/            PBZFM.dll 源码（pbzfm 入口 / overlay DComp 渲染 / input 钩子 /
                game_window 定位与子类化 / config / i18n / menu）
host/           测试宿主（模拟 UnrealWindow + 脚本输入 + 截图）
assets/         随包分发文件（PBZFM.ini、languages/*.json、README.txt）
tools/          build.bat / pack.ps1
third_party/    ImGui 1.92.1（vendored）、stb_image_write.h
docs/           自测截图
```

## 下一阶段（按优先级）

1. **实机验证**：游戏内 F8、FSR3 帧生成开着跑、真独占全屏/多显示器边角。
2. **注册 API**（C ABI 导出 + SDK 头，对齐 SKSEMenuFramework.h 形态）：
   `SetSection / AddSectionItem(渲染回调) / AddWindow / AddHudElement / AddInputEvent /
   AddConfigSchema(设置项描述) / 翻译表注册`；客户端模组 `GetModuleHandleW(L"PBZFM.dll")`
   + `GetProcAddress` 接入（同进程）。
3. **设置 schema**：toggle/slider/keybind/enum/combo 通用控件 + 按 mod 写回 config.ini + 变更回调。
4. 主题 JSON、手柄导航、游戏暂停（复用 PBZPhotoMode 已验证的暂停栈）。
5. 把 PBZPhotoMode 的 ImGui 诊断面板接成第一个客户端（其 MenuView/MenuActions 快照桥直接复用）。
