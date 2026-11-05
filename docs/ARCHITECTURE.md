# 架构与实机铁律

> 摘自原 README《架构：为什么是 DirectComposition 独立覆盖窗》与《自测方式》章节，
> 内容保持原样；改动历史见 git。模组视角的接入文档见 [MOD_AUTHORS.md](MOD_AUTHORS.md)，
> 验证记录见 [VERIFIED.md](VERIFIED.md)。

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
- 截图两条路。① 直通（按两次，**可在游戏内"设置"页直接编辑**：点组合键重新捕获、× 删除、
  "添加组合键"追加——改完立即写回 PBZFM.ini 并热更新到钩子，无需重启）：菜单打开且游戏在
  前台时，PBZFM.ini `passthrough_keys`
  （默认 `Ctrl+P, Alt+F1`，按你工具的实际热键配置）列出的组合键不再被吞，而是打开一个
  "借用窗口"——键鼠整体放行，游戏镜头仍冻结（Raw Input 仍指向覆盖窗）、菜单仍在背后渲染。
  第一次按下开启借用（**画面无变化属正常**，框架同时合成完整组合键重放）；第二次真实按键
  直达工具——实测 PixPin/N卡 会过滤注入按键（LLKHF_INJECTED 被忽略，合成重放对它们无效，
  只放行末键又拿不到被吞掉的修饰键上下文），所以必须由用户再真实按一次；工具交回前台后
  借用自动关闭、输入隔离恢复，呼出键/Esc 在借用期间始终可关菜单。② 兜底：框架设置页自带
  "截取屏幕"按钮，BitBlt 合成后的桌面区域（游戏窗口范围，**含菜单界面本身**），PNG 存到
  ModRoot\Screenshots\；
- 配置：PBZFM.ini（容错解析，菜单保存写回）；多语言：languages/*.json（扁平 JSON，
  热切换，内嵌兜底，内置 zh-CN / en-US / ja-JP / ko-KR）；客户端模组的分区名/页签/
  设置项 label 以文本为 key 参与查表（查不到原样显示）；CJK 字体：按界面语言加载系统
  字体（中文/日文用微软雅黑，韩文 UI 优先 Malgun Gothic，ImGui 1.92 动态字体图集）；
  界面主题：玄墨鎏金（src/theme，取自游戏 HUD 的配色语言——玄墨底、银色发丝线、
  鎏金高亮、朱砂点缀，全直角 + 1px 细边框，标题栏金线/银括角/朱砂印章装饰，
  键位捕获态为朱砂；装饰经前景绘制列表绘制，规避 1.92 窗口列表的标题栏裁剪）。

## 自测方式（host）

自测方式：`host/pbzfm_host.exe` 注册一个名为 `UnrealWindow` 的窗口（框架寻找的目标类）、
`LoadLibrary(PBZFM.dll)`（与 UE4SS 加载方式一致），脚本化 SendInput：F8 → 移动 → 点击 →
逐帧截图到 `build/shots/`。点击目标由 DLL 导出的 `PBZFM_GetDemoButton` 实时查询。
加 `--synthetic` 则改走 DLL 导出的合成输入通道（PBZFM_TestInjectMove/Button/ToggleMenu/
TestDispatchHotkey）：不经 OS 输入流，可在有人正在使用桌面时确定性回归。真实注入路径
依赖无人干扰的输入桌面——UAC/锁屏等安全桌面激活期间 LL 钩子收不到任何输入，属系统行为。
以上测试导出（`PBZFM_GetDemoButton` / `PBZFM_Test*`）只在 `-DPBZFM_TEST_HOOKS=ON` 的构建中
存在，正式包默认不导出（M8）；host 回归另含 M1 原子写与 M10 PNG alpha 断言。
非 synthetic 模式在收尾处追加直通场景（timer 15-18）：F8 开菜单 → 经
`PBZFM_TestSetPassthrough` 配置测试组合键 Ctrl+Alt+F9（刻意避开真实工具的热键）→
SendInput 和弦 → `PBZFM_TestPassthroughActive` 断言借用窗口开启 → 等宽限期 + 移动鼠标
断言隔离自动恢复；失败置退出码 1。
