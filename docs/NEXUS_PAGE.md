# Nexus Mods 主页介绍(NEXUS_PAGE)

> 用法:整段 BBCode 复制到 Nexus 模组页 Description 编辑器即可。
> 标题用 [size=3][b]…[/b][/size] —— Nexus 编辑器会把 [size=3][b] 按纯文本显示(实测)。
> 图片不上传正文:直接传到模组页 Images 标签;建议主图用实机截图
> (docs/prototype_menu_open.png 是 host 自测截图)。

---

## Summary 字段(页面简介,中英任选其一)

- EN: `A SKSE Menu Framework 3 style in-game mod menu for Phantom Blade Zero. F8 to open, per-mod pages, key rebinding, themes, 4 languages. Overlay rendering — no swap-chain hooks, safe with FSR3 frame generation.`
- 中文: `《影之刃零》游戏内模组菜单框架(类 Skyrim 的 SKSE Menu Framework 3):按 F8 呼出,各模组统一设置页、键位改绑、主题与四语言。独立覆盖窗渲染,不钩交换链,FSR3 帧生成环境安全。`

---

## 英文版(BBCode,主版本)

```bbcode
[size=4][b][color=#c8a44d]PBZ-Framework Menu[/color] — an in-game mod menu framework for Phantom Blade Zero[/b][/size]

A menu framework in the spirit of Skyrim's [url=https://github.com/QTR-Modding/SKSE-Menu-Framework-3]SKSE Menu Framework 3[/url], built for [url=https://www.nexusmods.com/phantombladezero]Phantom Blade Zero[/url] (UE 5.7) as a UE4SS C++ DLL mod. It gives mods a hotkey-summoned, in-game settings UI — with per-mod pages, key rebinding, themes, and multi-language — all editable from one unified interface.

[size=3][b]Highlights[/b][/size]
[list]
[*][b]One hotkey, every mod[/b] — press [b]F8[/b] (configurable) to open the menu; Esc or F8 closes it. Every registered mod shows up as its own section in the sidebar.
[*][b]Unified settings & key rebinding[/b] — checkboxes, sliders, dropdowns, text fields, color pickers and [b]in-game key capture[/b]. Configs are saved by the framework; mods read them back. Tested in-game: rebinding PBZPhotoMode's keys through this menu.
[*][b]Category tree sidebar[/b] — mods declare nested categories ("My Mod/Freeze Mode"); collapsible and remembered per session.
[*][b]Screenshot friendly[/b] — camera is frozen while the menu is open. A passthrough mode (default [b]Ctrl+P / Alt+F1[/b], two presses) lets your screenshot tool (PixPin, NVIDIA Overlay, …) capture the frozen scene; the settings page also has a built-in screen-capture button that saves PNGs to [b]Screenshots\[/b].
[*][b]Theming[/b] — ships with an ink-and-gold theme matching the game's HUD (dark base, hairline silver borders, gold highlights, cinnabar accents). Custom themes via [b]themes\<name>.json[/b], selectable in settings.
[*][b]4 languages, hot-switchable[/b] — 简体中文 / English / 日本語 / 한국어.
[*][b]Mod author API[/b] — a zero-dependency C header lets any UE4SS C++ DLL mod register sections, settings pages, custom-drawn pages, hotkeys and textures. See below.
[/list]

[size=3][b]Why it's safe for your game (architecture)[/b][/size]
The menu is an [b]independent DirectComposition overlay window[/b] with its own private D3D11 device and ImGui 1.92. It [b]never touches the game's swap chain or D3D objects[/b] — no Present hooks, no engine internals — so it is safe alongside [b]FSR3 frame generation and NVIDIA Streamline[/b], and friendly to the game's custom Shipping engine. Input is handled with low-level hooks: while the menu is open the game camera is frozen and all keyboard/mouse input feeds the menu; closing it restores everything instantly. Verified in-game at 2560x1440 with FSR3 frame generation on, and confirmed to coexist with PBZPhotoMode (F2 photo mode + F8 menu in the same session).

[size=3][b]For mod authors (C API / SDK)[/b][/size]
Include [b]pbzfm.h[/b] (shipped in the download under [b]PBZFM/sdk/[/b]) — the only external header is [b]windows.h[/b]; every function resolves at runtime via GetProcAddress, no import library, no bundled ImGui needed.
[list]
[*][b]ABI stability[/b] — a required core plus optional exports: old headers keep working against new framework versions; new exports degrade to NULL on old ones. Exported names and signatures, once released, never change.
[*][b]Sections & nested categories[/b] — declare trees like "My Mod/Sub Page" with a single call; sidebar renders the tree automatically.
[*][b]Schema settings pages[/b] — bool/int/float/text/enum/keyboard/key-combo widgets, change callbacks, per-mod JSON stores managed by the framework.
[*][b]Hotkeys[/b] — single keys and modifier combos, delivered even while the menu is open.
[*][b]Menu events, textures, i18n[/b] — open/close events on the render thread, stb_image texture loading, and label lookup through the framework's language tables (fallback: plain text).
[/list]
Full guide and a compilable reference implementation ([b]demo_mod.cpp[/b]) are in the download.

[size=3][b]Requirements[/b][/size]
[list=1]
[*][url=https://www.nexusmods.com/phantombladezero/mods/5]UE4SS for PBZ[/url] (KintaroModding fork) — install that first.
[*]This mod, installed with a mod manager or manually (see below).
[/list]

[size=3][b]Installation[/b][/size]
[list=1]
[*]Install [url=https://www.nexusmods.com/phantombladezero/mods/5]UE4SS for PBZ[/url].
[*]Install this mod's zip with your mod manager, or unzip it so you get:
[code]<game>\platform\steam\ue4ss\Mods\PBZFM\dlls\PBZFM.dll
<game>\platform\steam\ue4ss\Mods\PBZFM\enabled.txt[/code]
[*]Launch the game and press [b]F8[/b].
[/list]
Uninstall: delete the [b]PBZFM[/b] folder. No game files are ever modified.

[size=3][b]Troubleshooting[/b][/size]
First check the log at [b]Documents\PBZ\Logs\PBZFM.log[/b] (created automatically; falls back to [b]ue4ss\Mods\PBZFM\PBZFM.log[/b] if Documents is not writable). In UE4SS.log you should see no PBZFM warnings.

[size=3][b]Known limitations[/b][/size]
[list]
[*]Gamepad navigation is developed but temporarily shelved at the author's request — not included in the current package.
[*]The screenshot passthrough is a "two-press" design: the first press opens the borrow window, the second real press triggers your tool (PixPin / NVIDIA overlay ignore injected keys).
[*]Not yet verified: true exclusive fullscreen, multi-monitor, HDR, Steam Overlay.
[/list]

[size=3][b]Permissions & credits[/b][/size]
MIT licensed. The SDK header carries [b]zero obligation[/b] for client mods — including it and talking to PBZFM.dll at runtime does not make your mod a derivative; license it however you like.
Credits: [url=https://github.com/ocornut/imgui]Dear ImGui[/url], [url=https://github.com/nothings/stb]stb_image[/url], [url=https://github.com/KintaroModding/UE4SS-RE-RE-for-Atsors-PBZ]UE4SS for PBZ[/url], and [url=https://github.com/lin-414/PBZPhotoMode]PBZPhotoMode[/url] whose field-tested findings shaped the overlay approach. Inspired by SKSE Menu Framework 3.
[/size]
```

---

## 中文版(BBCode,作为英文版下方的补充段落,或单独发布)

```bbcode
[size=4][b][color=#c8a44d]PBZ-Framework Menu[/color] —— 《影之刃零》游戏内模组菜单框架[/b][/size]

一个类似 Skyrim 的 [url=https://github.com/QTR-Modding/SKSE-Menu-Framework-3]SKSE Menu Framework 3[/url] 的《影之刃零》(UE 5.7)模组菜单框架,以 UE4SS C++ DLL 模组形式运行:给模组提供快捷键呼出的游戏内设置界面、键位改绑、主题与多语言,所有模组的设置集中在一个菜单里编辑。

[size=3][b]特性[/b][/size]
[list]
[*][b]一个快捷键呼出所有模组[/b] —— 默认按 [b]F8[/b] 开/关菜单(可改),每个已注册模组在侧边栏有自己的分区。
[*][b]统一设置与键位改绑[/b] —— 复选框、滑条、下拉、文本框、取色器与[b]游戏内键位捕获[/b];配置由框架统一保存。已实机验证:在菜单里改绑 PBZPhotoMode 的键位并正常保存。
[*][b]侧边栏分类树[/b] —— 模组可用 "我的模组/冻结方式" 式路径声明任意层级分类,可展开收起(会话内记忆)。
[*][b]截图友好[/b] —— 菜单打开时游戏镜头冻结;截图直通(默认 [b]Ctrl+P / Alt+F1[/b],按两次)放行 PixPin / N卡overlay 等截图工具对冻结画面选区;设置页自带"截取屏幕"按钮,PNG 存到 [b]Screenshots\[/b]。
[*][b]主题[/b] —— 内置"玄墨鎏金"主题(取自游戏 HUD 配色:玄墨底、银色发丝线、鎏金高亮、朱砂点缀);可用 [b]themes\<name>.json[/b] 自定义并在设置页切换。
[*][b]四语言热切换[/b] —— 简体中文 / English / 日本語 / 한국어。
[*][b]模组作者 API[/b] —— 零依赖 C 头文件,任何 UE4SS C++ DLL 模组都能注册分区、设置页、自绘页、热键与纹理,见下。
[/list]

[size=3][b]为什么对游戏是安全的(架构)[/b][/size]
菜单是[b]独立的 DirectComposition 覆盖窗[/b],带私有 D3D11 设备与 ImGui 1.92 渲染,[b]完全不碰游戏的交换链与 D3D 对象[/b]——没有 Present 钩子、不依赖引擎内部,因此与 [b]FSR3 帧生成、NVIDIA Streamline[/b] 共存安全,对本作的定制 Shipping 引擎也友好。输入经低级钩子处理:菜单打开时镜头冻结、键鼠全部喂给菜单,关闭即恢复。已实机验证:2560x1440 + FSR3 帧生成稳定运行,与 PBZPhotoMode 同局共存(F2 摄影模式 + F8 菜单)。

[size=3][b]模组作者接入(C API / SDK)[/b][/size]
下载包内附 [b]pbzfm.h[/b](路径 [b]PBZFM/sdk/[/b]),唯一外部头是 [b]windows.h[/b];所有函数运行时经 GetProcAddress 解析,不需要导入库、不需要自带 ImGui。
[list]
[*][b]ABI 兼容策略[/b] —— 必需核心 + 可选导出:旧头文件配新框架继续工作,新功能在旧框架上退化为 NULL 指针(判空调用);导出名与签名一旦发布永不更改。
[*][b]分区与嵌套分类[/b] —— "我的模组/子页面" 一句路径声明,侧边栏自动成树。
[*][b]schema 设置页[/b] —— 布尔/数值/文本/枚举/单键/组合键控件、变更回调,每模组 JSON 配置由框架托管。
[*][b]热键[/b] —— 单键与修饰键组合键,菜单打开时依然分发。
[*][b]菜单事件、纹理、多语言[/b] —— 渲染线程开/关事件、stb_image 纹理加载、label 参与框架语言查表(查不到原样显示)。
[/list]
完整接入指南与可编译参考实现([b]demo_mod.cpp[/b])都在下载包内。

[size=3][b]前置要求[/b][/size]
[list=1]
[*][url=https://www.nexusmods.com/phantombladezero/mods/5]UE4SS for PBZ[/url](KintaroModding fork)—— 请先安装。
[*]本模组:用模组管理器安装,或手动解压(见下)。
[/list]

[size=3][b]安装[/b][/size]
[list=1]
[*]安装 [url=https://www.nexusmods.com/phantombladezero/mods/5]UE4SS for PBZ[/url]。
[*]用模组管理器安装本模组 zip,或手动解压出:
[code]<游戏目录>\platform\steam\ue4ss\Mods\PBZFM\dlls\PBZFM.dll
<游戏目录>\platform\steam\ue4ss\Mods\PBZFM\enabled.txt[/code]
[*]启动游戏,按 [b]F8[/b]。
[/list]
卸载:删除 [b]PBZFM[/b] 文件夹即可,游戏文件零改动。

[size=3][b]排障[/b][/size]
先看日志 [b]文档\PBZ\Logs\PBZFM.log[/b](自动创建;"文档"目录不可写时回落到 [b]ue4ss\Mods\PBZFM\PBZFM.log[/b])。UE4SS.log 中不应出现 PBZFM 相关警告。

[size=3][b]已知边界[/b][/size]
[list]
[*]手柄导航已开发完成,但应用户要求暂时搁置,未随包发布。
[*]截图直通是"两次按键"设计:第一下开启借用窗口,第二下真实触发截图工具(PixPin / N卡会忽略注入按键)。
[*]尚未验证:真独占全屏、多显示器、HDR、Steam Overlay。
[/list]

[size=3][b]授权与致谢[/b][/size]
MIT 许可。SDK 头对客户端模组[b]零义务[/b]——include 该头并与运行中的 PBZFM.dll 通信,不构成衍生作品,可用任意许可证分发自己的模组。
致谢:[url=https://github.com/ocornut/imgui]Dear ImGui[/url]、[url=https://github.com/nothings/stb]stb_image[/url]、[url=https://github.com/KintaroModding/UE4SS-RE-RE-for-Atsors-PBZ]UE4SS for PBZ[/url],以及 [url=https://github.com/lin-414/PBZPhotoMode]PBZPhotoMode[/url](其经实机取证的结论奠定了覆盖窗路线)。灵感来自 SKSE Menu Framework 3。
```

---

## 发布建议

- **Mod 名称**:PBZ-Framework Menu;**分类**:User Interface;**标签**:Menu、Framework、UE4SS。
- **图片**:不上传正文,直接传模组页 Images 标签;主图建议实机截图(2560x1440、FSR3 开),再补 2-3 张:分类树展开、键位捕获态(朱砂)、截图直通场景。
- **版本日志**:把 README《0.5.2》列表直接贴进 Changes,英中双语或英文即可。
- Nexus 编辑器会把 [size=3][b] 渲染成纯文本(实测),所以正文标题统一用 [size=3][b]…[/b][/size]。
