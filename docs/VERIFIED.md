# 验证记录

> 承接原 README《已验证》表。约定不变：host 自测可自动化的项标 host 回归；实机
> 结论由用户安装 dist zip 后测试并录入。

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
| 截图直通状态机（Ctrl+Alt+F9 开借用窗口→键鼠放行→宽限期+前台回游戏自动恢复隔离；呼出键始终可关菜单）host 回归 | ✅ 2026-11-01 |
| 截图直通键菜单内编辑（点击组合键重捕获带修饰键、× 删除、添加追加；改完即时生效并写回 ini；host 设置页截图目检） | ✅ 2026-11-02 |

## 0.4.0 新增（host 回归通过；实机复核待做）

| 项 | 结果 |
| --- | --- |
| ABI v2：必需核心 + 可选导出（旧头文件/demo_mod 零改动继续工作） | ✅ 2026-11-01 host 回归 |
| 菜单开关事件（OnMenuEvent）分发（渲染线程，开/关各一次） | ⏳ 待实机（host 未断言） |
| 组合键热键 AddHotkeyEx(vk+mods 严格匹配) / RemoveHotkey | ⏳ 待实机（host 未断言） |
| schema 组合键捕获（SCH_KEYCOMBO，修饰键 + 末键） | ⏳ 待实机（host 未断言） |
| 分区生命周期 RenameSection / DeleteSection / RemoveSectionItem | ⏳ 待实机（host 未断言） |
| 主题 JSON（themes\<name>.json 覆盖颜色/圆角/边框；设置页选择器） | ⏳ 待实机（host 未断言） |
| 纹理 API LoadTexture/Image/DisposeTexture（stb_image，渲染线程上传） | ⏳ 待实机（host 未断言） |
| 新 Widget：InputText/CollapsingHeader/Selectable/ProgressBar/ColorEdit3 | ⏳ 待实机（host 未断言） |
| DLL 版本资源（0.4.0）+ i18n_test 进 CTest（32 条/语言 ALL OK） | ✅ 2026-11-01 |

## 0.5.0 新增（host 回归通过；实机复核待做）

| 项 | 结果 |
| --- | --- |
| 侧边栏分类树（单页模组单行、展开箭头、**默认全部收起**、点击即切换展开/收起、选中金色 + 朱砂条） | ⏳ 待实机（host 截图覆盖） |
| 嵌套路径 API：SetSectionPath / ConfigCreatePath / AddSectionItemPath（'/' 分隔、'\\' 转义、中间节点自动创建） | ✅ demo_mod "演示模组/高级参数" 经 host 回归 |
| 展开状态仅会话内、默认收起（不写 PBZFM.ini，按用户选择） | ✅ 设计确认 |
| v1 兼容：单段名 Rename/Delete/Remove 与旧调用不变；GetSectionCount/Rect 仍按根行枚举 | ✅ host 回归全绿 |

## 0.5.1 新增（host 已验证；实机复核待做）

| 项 | 结果 |
| --- | --- |
| 面板缩放联动：拖拽调整面板大小时字体、侧栏与按钮宽度按面板宽度等比缩放（字体整体封顶 0.7–1.8 倍），并受窗口最小/最大尺寸限制 | ✅ 2026-11-02 host 截图对比（默认/放大/最小三态） |

## 方案 C：引擎内输入（feat/engine-input）

| 项 | 结果 |
| --- | --- |
| 后端策略纯判定（auto/engine/os × 引擎就绪）+ `input_mode` ini 往返 | ✅ 2026-11-05 framework_test |
| `PBZFM_ENGINE_PORT=OFF`：导入表无 UE4SS，行为与 0.5.2 一致 | ✅ 2026-11-05 构建 + ctest 全绿 |
| `PBZFM_ENGINE_PORT=ON`：真实 UE4SS 头 + `UE4SS.lib` 编译链接通过；导入表含 UE4SS.dll + 动态 CRT | ✅ 2026-11-05 build_engine.bat + ctest 全绿 |
| 实机：菜单开着时录屏/截图/游戏栏热键仍有效 | ⏳ 待实机（本机游戏目录已卸载模组） |
| 实机：菜单开着时 WASD/鼠标不动；关闭即恢复；反复开关 20 次 + 切图不过期 | ⏳ 待实机 |
| 窗口消息输入通道（引擎模式无全局钩子）：键盘/呼出键/键位捕获走游戏 WndProc，鼠标走覆盖窗 | ✅ 2026-11-05 两套构建 + ctest；实机行为待验证 |
| 冻结重新施加：WM_ACTIVATE/WM_SETFOCUS 立即补 + 200ms 读回校验（修 N 卡浮层抢焦点后左键漏给 PhotoMode） | ✅ 2026-11-05 两套构建 + host 回归；实机待复核 |
| 引擎模式也做进程级 Raw Input 改指向（鼠标+键盘），挡住绕过 PlayerController 的输入 | ✅ 2026-11-05 host 回归（steal/restore 全链路，sinks removed 正常） |
| Raw Input 归属判定改用 hwndTarget（修 4Hz 重抢导致还原表丢游戏鼠标、关菜单后相机死） | ✅ 2026-11-05 三套构建 + host 回归 |
| 覆盖窗 WDA_EXCLUDEFROMCAPTURE（N 卡不再把菜单当录制目标，关菜单不再结束录制） | ✅ 2026-11-05 host 日志可见；实机待复核 |
