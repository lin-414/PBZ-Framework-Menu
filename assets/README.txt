PBZ-Framework Menu (0.4.0)
=============================================

《影之刃零》模组菜单框架的第一个可运行原型：
DirectComposition 独立覆盖窗 + ImGui，不钩游戏交换链（帧生成安全），
不依赖游戏引擎内部（Shipping 定制引擎黑盒友好）。

0.4.0 新增：
  - 侧边栏搜索框（输入即筛选模组）
  - 设置页主题选择器：themes\<name>.json 可覆盖界面配色（详见仓库 docs/）
  - 模组 API v2：菜单开关事件 / 组合键热键 / 分区管理 / 纹理（模组作者见 sdk\pbzfm.h）

安装（需要已装 UE4SS for PBZ）：
  1. 把 PBZFM 文件夹整个放进 <游戏目录>\platform\steam\ue4ss\Mods\
     形成 ue4ss\Mods\PBZFM\dlls\PBZFM.dll 与 ue4ss\Mods\PBZFM\enabled.txt
  2. 启动游戏，按 F8（默认）呼出/关闭菜单。

游戏内验证清单：
  [ ] 游戏正常启动，无报错
  [ ] 按 F8 菜单出现，再按关闭
  [ ] 菜单打开时：鼠标可点控件、滚轮可用、游戏键鼠无响应
  [ ] 菜单打开时按 Win 切到开始菜单：系统光标可移动、可点击；切回游戏后镜头仍冻结、菜单光标正常
  [ ] 菜单打开时按两下你的截图热键（passthrough_keys，默认 Ctrl+P / Alt+F1）：
      第一下开启借用（画面不变属正常），第二下 PixPin/N卡 正常选区截图，期间游戏镜头
      保持冻结；结束后回到游戏，F8/Esc 仍可正常关闭菜单
  [ ] 菜单关闭后游戏输入完全恢复
  [ ] 拖动滑条/切语言/点"保存配置"后，PBZFM.ini 出现在 ue4ss\Mods\PBZFM\ 下，
      PBZFM.log 出现在 Documents\PBZ\Logs\（系统"文档"目录，自动创建）下（排障先看日志）
  [ ] FSR3 帧生成开着画面稳定、无闪烁无卡死

安装检查（重要）：
  UE4SS.log 里不应再出现 "Failed to find exported mod lifecycle functions for mod PBZFM"；
  出现说明装的还是旧包。正常情况 UE4SS.log 无 PBZFM 相关警告，
  而 Documents\PBZ\Logs\PBZFM.log 会出现 "loading / attached to game window / prototype ready"
  （该目录不可写时回落到 ue4ss\Mods\PBZFM\PBZFM.log）。

已知原型边界：
  - 截图直通是"两次按键"：第一次开启借用、第二次真实触发工具（PixPin/N卡实测忽略
    注入按键,合成重放无法替代真实按下）；passthrough_keys 按你工具的实际热键配置,
    改动后重启游戏生效
  - 手柄导航未随包发布（功能已开发，暂时搁置；后续版本可能回归）
  - 尚未验证：真独占全屏、多显示器、HDR、Steam Overlay 等环境边角

卸载：删除 ue4ss\Mods\PBZFM 文件夹即可，游戏文件零改动。