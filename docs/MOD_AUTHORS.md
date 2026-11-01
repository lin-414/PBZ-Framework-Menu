# 模组作者接入指南（C API / SDK）

> 承接原 README《模组作者接入指南》章节并按 0.4.0 API 更新。零依赖接入头
> [sdk/pbzfm.h](../sdk/pbzfm.h)（随 dist zip 分发于 PBZFM/sdk/），唯一外部头是
> windows.h；完整可编译参考 [host/demo_mod.cpp](../host/demo_mod.cpp)。

任何加载进游戏进程的 DLL（实践中就是 UE4SS C++ DLL 模组，PBZPhotoMode 形态）都可以
向菜单注册自己的侧边栏分区、设置页、自绘页与游戏内热键。接入**零构建依赖**：只
include [sdk/pbzfm.h](sdk/pbzfm.h)（dist zip 内路径 `PBZFM/sdk/pbzfm.h`），唯一
外部头是 `windows.h`；所有函数运行时经 `GetProcAddress` 解析，不链接导入库、
不需要自带 ImGui。

### 版本与兼容策略（0.4.0 起）

- 头文件里的 `PBZFM_ABI_VERSION`（当前 1）是**本头文件要求的最低框架版本**；
  `PBZFM_GetAbiVersion()` 返回**运行中框架的 API 版本**（0.5.0 / API v3）。加载条件是
  框架版本 ≥ 头文件要求版本——旧头文件配新框架继续工作。
- 必需核心（v1 的全部导出）解析失败才算加载失败；0.4.0 新增的导出按**可选**解析，
  对应函数指针在旧框架上为 NULL——调用前判空（`if (api.RemoveHotkey) …`）。
- 已发布的导出名与签名永不更改；破坏性变更才会再升 ABI 版本。

### 工作原理与前提

- 框架本体是 UE4SS 模组 `ue4ss\Mods\PBZFM\dlls\PBZFM.dll`。你的模组调用内联的
  `PBZFM_LoadApi(&api)`：内部 `GetModuleHandleW(L"PBZFM.dll")` 找到框架、解析函数指针
  ——必需核心（v1 导出）全部解析成功且运行时 API 版本 ≥ 头文件的
  `PBZFM_ABI_VERSION` 才算成功；0.4.0 新增导出按可选解析，缺失时对应指针为 NULL。
  任一必需环节失败返回 0（语义见上方《版本与兼容策略》）。
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
| SetSection / ConfigCreate / ConfigAdd* / AddHotkey(Ex) / OnMenuEvent 注册 | 任意（自己的线程） | 绝不在 DllMain；框架未加载就轮询；v2 导出判空后调用 |
| 自定义页渲染回调（AddSectionItem 的 fn） | 框架渲染线程 | 仅菜单打开且你的页签选中时每帧执行；保持快速、绝不阻塞 |
| 设置变更回调（ConfigOnChange 的 fn） | 框架渲染线程 | 每次提交编辑触发；值已更新，用 Get* 读回 |
| 游戏热键回调（AddHotkey/AddHotkeyEx 的 fn） | 框架输入钩子线程 | 需要游戏线程的数据自行 marshal |
| 菜单开关事件（OnMenuEvent 的 fn） | 框架渲染线程 | 打开/关闭各触发一次，open=1/0 |

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
- **组合键键位（0.4.0）**：`ConfigAddKeyCombo(cfg, key, label, defVk, defMods)` 声明
  带修饰键的键位（`PBZFM_MOD_CTRL | PBZFM_MOD_ALT` 等，位掩码；存储值为
  `vk | (mods << 8)`）。菜单内捕获：按住修饰键再按末键完成。读取用
  `GetKey`（vk）+ `GetKeyMods`（位掩码），程序化写值用 `SetKeyCombo`，
  显示名用 `KeyName(vk, mods, buf, bufLen)`（如 "Ctrl+Alt+P"）。
- `GetBool / GetFloat / GetInt / GetEnum / GetKey` 读当前值；
  `SetBool / SetFloat / SetInt / SetEnum / SetKey` 可在你的任意线程程序化写值，
  语义与界面编辑一致（标脏 + 触发变更回调）。
- `ConfigOnChange` 可不注册：不注册时框架照样渲染与持久化，只是没有实时生效钩子。

### 分区与页签生命周期（0.4.0）

`SetSection` 注册的分区是全进程共享命名空间里的实体，现在可以运行时管理
（0.5.0 起三个函数的第一个参数是**路径**，单段路径即顶层分区，v1 按名字调用不变）：

```cpp
api.RenameSection("旧名", "新名");            // 1 = 成功；目标重名/来源缺失 = 0
api.DeleteSection("名字");                    // 连同其下所有页签
api.RemoveSectionItem("分区名", "页签名");     // 只删一页
```

- 删除分区**不删**它的 schema 数据文件（data/<store>.json 保留，重新注册同名
  store 会重新加载）；
- 删除/改名后，本线程的"当前分区"失效——继续注册前重新 `SetSection`；
NaN```cpp
if (api.SetSectionPath) api.SetSectionPath("我的模组/冻结方式");
// 或一步到位（等价，且不改变"当前分区"）：
if (api.ConfigCreatePath) {
    if (PBZFM_Config adv = api.ConfigCreatePath("author.mymod", "我的模组/冻结方式")) {
        api.ConfigAddBool(adv, "freeze_world", "冻结世界", 1);
    }
}
if (api.AddSectionItemPath) api.AddSectionItemPath("我的模组/关于", RenderAbout, nullptr);
```

- 路径语法：`/` 分隔段，`\` 转义下一字节（段内字面 `/` 写 `\/）；单段路径
  即顶层分区——`SetSectionPath("A")` 与 `SetSection("A")` 等价；
- 缺失的中间节点自动创建（空节点仅作分组，不渲染内容）；
- 页面内容挂在路径节点上：`ConfigCreatePath(store, path)` 的页签名取路径
  末段；同一节点也可用 `AddSectionItemPath` 挂自定义页；
- **树形交互**：只有"单页且无子节点"的分区渲染成单行（简单模组外观与旧版完全
  一致）；有子节点或多页的分区带展开箭头，点击分类右侧只显示该分类的内容，
  再点一次已选中的父节点会收起/展开；展开状态**仅本次会话**有效，不写配置；
- v1 API 与路径 API 完全互通：`SetSection("我的模组")` 后照常 `ConfigCreate`，
  得到的页面同样是"我的模组"的子行；注意从 0.5.0 起分区名里的 `/` 会被解释为
  层级分隔（此前它是名字的一部分），名字含斜杠的存量分区请改用转义或换名。

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

Widget API 仅在渲染回调内有效（均为 ImGui 对应控件的薄封装，参数直接透传）：
`Text / TextDisabled / Separator / SameLine / Button / Checkbox / SliderFloat /
SliderInt / Combo / LastItemRect`，0.4.0 新增 `InputText(label, buf, bufSize)` /
`CollapsingHeader` / `Selectable(label, selected)` / `ProgressBar(fraction, width,
overlay)` / `ColorEdit3(label, rgb[3])`。回调卡住 = 整个菜单卡住。

### 菜单开关事件与纹理（0.4.0）

```cpp
static void OnMenuEvent(int open, void*) { /* 渲染线程；open=1 打开 / 0 关闭 */ }
int ev = api.OnMenuEvent ? api.OnMenuEvent(OnMenuEvent, nullptr) : 0;
// api.RemoveMenuEvent(ev);  // 不再需要时注销

void* tex = api.LoadTexture ? api.LoadTexture("textures/logo.png") : nullptr;
// 自定义页里：api.Image(tex, 96, 96);   // 未加载完成前不绘制
// api.DisposeTexture(tex);              // 不用时释放显存
```

- 事件在覆盖窗可见性切换后于渲染线程触发，每开/关各一次；不用轮询
  `IsMenuOpen` 了（该导出保留）。
- 纹理支持 PNG/JPG/BMP/TGA（stb_image）；相对路径以框架 ModRoot
  （ue4ss\Mods\PBZFM）为基准，绝对路径按原样使用。同一路径重复 Load 共享
  同一纹理；`DisposeTexture` 后再 Load 会重新加载。

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
- **组合键热键（0.4.0）**：`AddHotkeyEx(vk, mods, fn, ud)` 返回注册句柄，
  修饰键**严格匹配**（多按一个 Shift 不会触发 Ctrl+Alt+P，与截图直通同一语义）；
  `RemoveHotkey(id)` 注销。旧 `AddHotkey` 等价于 mods=0，签名不变。
- 菜单打开期间已注册热键照常分发（见上）；组合键只在完整和弦匹配时吞键，
  单独按末键（无修饰）不会触发也不吞。

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
| 框架配置（呼出键 / 语言 / UI 缩放与不透明度 / 主题） | `ue4ss\Mods\PBZFM\PBZFM.ini` |
| 你的设置数据 | `ue4ss\Mods\PBZFM\data\<store>.json` |
| 翻译表 | `ue4ss\Mods\PBZFM\languages\<lang>.json` |
| 日志（排障先看） | `Documents\PBZ\Logs\PBZFM.log`（不可写时回落 `Mods\PBZFM\PBZFM.log`） |
| 框架"截取屏幕"按钮产物 | `ue4ss\Mods\PBZFM\Screenshots\` |

另有 `PBZFM_IsMenuOpen()` 供客户端轮询菜单开关（0.4.0 起更推荐上面的 `OnMenuEvent` 事件）；
`PBZFM_GetSectionCount /`PBZFM_GetSectionCount /
GetSectionNameUtf8 / GetSectionRect / GetLastSchemaItemRect` 是 host 自动化测试用的
几何查询，常规模组不需要。
