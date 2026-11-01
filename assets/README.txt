PBZ-Framework Menu (0.2.0)
=============================================

《影之刃零》模组菜单框架的第一个可运行原型：
DirectComposition 独立覆盖窗 + ImGui，不钩游戏交换链（帧生成安全），
不依赖游戏引擎内部（Shipping 定制引擎黑盒友好）。

安装（需要已装 UE4SS for PBZ）：
  1. 把 PBZFM 文件夹整个放进 <游戏目录>\platform\steam\ue4ss\Mods\
     形成 ue4ss\Mods\PBZFM\dlls\PBZFM.dll 与 ue4ss\Mods\PBZFM\enabled.txt
  2. 启动游戏，按 F8（默认）呼出/关闭菜单。

游戏内验证清单：
  [ ] 游戏正常启动，无报错
  [ ] 按 F8 菜单出现，再按关闭
  [ ] 菜单打开时：鼠标可点控件、滚轮可用、游戏键鼠无响应
  [ ] 菜单关闭后游戏输入完全恢复
  [ ] 拖动滑条/切语言/点"保存配置"后，PBZFM.ini 与 PBZFM.log 出现在
      ue4ss\Mods\PBZFM\ 下（排障先看日志）
  [ ] FSR3 帧生成开着画面稳定、无闪烁无卡死

安装检查（重要）：
  UE4SS.log 里不应再出现 "Failed to find exported mod lifecycle functions for mod PBZFM"；
  出现说明装的还是旧包。正常情况 UE4SS.log 无 PBZFM 相关警告，
  而 ue4ss\Mods\PBZFM\PBZFM.log 会出现 "loading / attached to game window / prototype ready"。

已知原型边界：
  - 尚无模组注册 API（下一阶段：SetSection/AddSectionItem/设置 schema）
  - 尚未验证：真独占全屏、多显示器、HDR、Steam Overlay 等环境边角
  - 手柄导航未接入

卸载：删除 ue4ss\Mods\PBZFM 文件夹即可，游戏文件零改动。