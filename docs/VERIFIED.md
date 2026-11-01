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

## 0.4.0 新增（host 回归通过；实机复核待做）

| 项 | 结果 |
| --- | --- |
| ABI v2：必需核心 + 可选导出（旧头文件/demo_mod 零改动继续工作） | ✅ 2026-11-01 host 回归 |
| 菜单开关事件（OnMenuEvent）分发（渲染线程，开/关各一次） | ⏳ 待实机（host 未断言） |
| 组合键热键 AddHotkeyEx(vk+mods 严格匹配) / RemoveHotkey | ⏳ 待实机（host 未断言） |
| schema 组合键捕获（SCH_KEYCOMBO，修饰键 + 末键） | ⏳ 待实机（host 未断言） |
| 分区生命周期 RenameSection / DeleteSection / RemoveSectionItem | ⏳ 待实机（host 未断言） |
| 侧边栏搜索框（按显示名/原名过滤，选中项不跳变） | ⏳ 待实机（host 未断言） |
| 主题 JSON（themes\<name>.json 覆盖颜色/圆角/边框；设置页选择器） | ⏳ 待实机（host 未断言） |
| 纹理 API LoadTexture/Image/DisposeTexture（stb_image，渲染线程上传） | ⏳ 待实机（host 未断言） |
| 新 Widget：InputText/CollapsingHeader/Selectable/ProgressBar/ColorEdit3 | ⏳ 待实机（host 未断言） |
| DLL 版本资源（0.4.0）+ i18n_test 进 CTest（32 条/语言 ALL OK） | ✅ 2026-11-01 |

## 0.5.0 新增（host 回归通过；实机复核待做）

| 项 | 结果 |
| --- | --- |
| 侧边栏分类树（单页模组单行、展开箭头、再点收起、选中金色 + 朱砂条） | ⏳ 待实机（host 截图覆盖默认视图） |
| 嵌套路径 API：SetSectionPath / ConfigCreatePath / AddSectionItemPath（'/' 分隔、'\\' 转义、中间节点自动创建） | ✅ demo_mod "演示模组/高级参数" 经 host 回归 |
| 树形搜索过滤（子树匹配、过滤期间全展开） | ⏳ 待实机（host 未断言） |
| 展开状态仅会话内（不写 PBZFM.ini，按用户选择） | ✅ 设计确认 |
| v1 兼容：单段名 Rename/Delete/Remove 与旧调用不变；GetSectionCount/Rect 仍按根行枚举 | ✅ host 回归全绿 |
