#include "i18n.h"
#include "common.h"
#include <cstdio>
#include <cstdlib>

namespace pbzfm {

static const char* kJsonZh =
R"json({
  "title": "PBZ-Framework Menu",
  "framework_section": "PBZ-Framework Menu",
  "page_settings": "设置",
  "hint": "DirectComposition 独立覆盖窗",
  "btn_test": "测试按钮",
  "count": "计数",
  "sec_display": "显示",
  "sec_demo": "演示参数",
  "sec_keys": "键位",
  "language": "语言",
  "ui_scale": "界面缩放",
  "ui_opacity": "不透明度",
  "fov": "视野 (FOV)",
  "freeze": "时间冻结",
  "quality": "画质预设",
  "q_low": "低",
  "q_medium": "中",
  "q_high": "高",
  "q_custom": "自定义",
  "toggle_key": "打开/关闭菜单",
  "capturing": "按下新按键…",
  "screenshot": "截取屏幕",
  "shot_saved": "已保存到 Screenshots 文件夹",
  "passthrough": "截图直通热键",
  "passthrough_hint": "菜单打开期间按下这些键:第一次开启输入借用(画面不变属正常),再按一次同样的键,PixPin/N卡 即正常截图(期间镜头保持冻结),回到游戏后自动恢复。在 PBZFM.ini [framework] passthrough_keys 配置,逗号分隔,如 Ctrl+P, Alt+F1",
  "save": "保存配置",
  "close_menu": "关闭菜单 (Esc)",
  "saved": "已保存到 PBZFM.ini",
  "search": "搜索",
  "search_hint": "输入以筛选模组…",
  "theme": "主题",
  "theme_builtin": "玄墨鎏金",
  "select_hint": "在左侧选择一个分类"
})json";

static const char* kJsonEn =
R"json({
  "title": "PBZ-Framework Menu",
  "framework_section": "PBZ-Framework Menu",
  "page_settings": "Settings",
  "hint": "Independent DirectComposition overlay",
  "btn_test": "Test button",
  "count": "clicks",
  "sec_display": "Display",
  "sec_demo": "Demo settings",
  "sec_keys": "Key bindings",
  "language": "Language",
  "ui_scale": "UI scale",
  "ui_opacity": "Opacity",
  "fov": "Field of view (FOV)",
  "freeze": "Freeze time",
  "quality": "Quality preset",
  "q_low": "Low",
  "q_medium": "Medium",
  "q_high": "High",
  "q_custom": "Custom",
  "toggle_key": "Toggle menu",
  "capturing": "Press a new key...",
  "screenshot": "Capture screen",
  "shot_saved": "Saved to the Screenshots folder",
  "passthrough": "Screenshot-tool passthrough",
  "passthrough_hint": "While the menu is open, the first press of these chords opens an input borrow (no visual change is normal) and the SECOND press reaches PixPin/NVIDIA for real (the camera stays frozen); isolation resumes when you return to the game. Configure passthrough_keys in PBZFM.ini [framework], comma-separated, e.g. Ctrl+P, Alt+F1",
  "save": "Save settings",
  "close_menu": "Close menu (Esc)",
  "saved": "Saved to PBZFM.ini",
  "search": "Search",
  "search_hint": "Type to filter mods...",
  "theme": "Theme",
  "theme_builtin": "Ink & Gold",
  "select_hint": "Pick a category on the left"
})json";

static const char* kJsonJp =
R"json({
  "title": "PBZ-Framework Menu",
  "framework_section": "PBZ-Framework Menu",
  "page_settings": "設定",
  "hint": "独立した DirectComposition オーバーレイ",
  "btn_test": "テストボタン",
  "count": "クリック数",
  "sec_display": "表示",
  "sec_demo": "デモ設定",
  "sec_keys": "キー設定",
  "language": "言語",
  "ui_scale": "UI スケール",
  "ui_opacity": "不透明度",
  "fov": "視野角 (FOV)",
  "freeze": "時間停止",
  "quality": "画質プリセット",
  "q_low": "低",
  "q_medium": "中",
  "q_high": "高",
  "q_custom": "カスタム",
  "toggle_key": "メニュー開閉",
  "capturing": "新しいキーを押してください…",
  "screenshot": "スクリーンショット",
  "shot_saved": "Screenshots フォルダに保存しました",
  "passthrough": "スクショツール直通キー",
  "passthrough_hint": "メニュー表示中にこのキーを押すと、1回目で入力借用が開き(見た目は変わりません)、2回目で PixPin/NVIDIA にキーが届き通常どおりキャプチャできます(カメラは凍結のまま)。PBZFM.ini [framework] passthrough_keys でカンマ区切りで指定(例: Ctrl+P, Alt+F1)",
  "save": "設定を保存",
  "close_menu": "メニューを閉じる (Esc)",
  "saved": "PBZFM.ini に保存しました",
  "search": "検索",
  "search_hint": "入力してモッドを絞り込む…",
  "theme": "テーマ",
  "theme_builtin": "玄墨鎏金",
  "select_hint": "左のカテゴリを選択してください"
})json";

static const char* kJsonKr =
R"json({
  "title": "PBZ-Framework Menu",
  "framework_section": "PBZ-Framework Menu",
  "page_settings": "설정",
  "hint": "독립 실행형 DirectComposition 오버레이",
  "btn_test": "테스트 버튼",
  "count": "클릭 수",
  "sec_display": "디스플레이",
  "sec_demo": "데모 설정",
  "sec_keys": "키 설정",
  "language": "언어",
  "ui_scale": "UI 배율",
  "ui_opacity": "불투명도",
  "fov": "시야각 (FOV)",
  "freeze": "시간 정지",
  "quality": "화질 프리셋",
  "q_low": "낮음",
  "q_medium": "보통",
  "q_high": "높음",
  "q_custom": "사용자 지정",
  "toggle_key": "메뉴 열기/닫기",
  "capturing": "새 키를 누르세요...",
  "screenshot": "화면 캡처",
  "shot_saved": "Screenshots 폴더에 저장했습니다",
  "passthrough": "스크린샷 도구 직통 키",
  "passthrough_hint": "메뉴가 열려 있는 동안 이 키를 누르면 첫 입력으로 입력 빌려쓰기가 열리고(화면 변화 없음), 두 번째 입력이 PixPin/NVIDIA 에 전달되어 정상 캡처됩니다(카메라는 정지 유지). PBZFM.ini [framework] passthrough_keys 에 쉼표로 구분해 지정(예: Ctrl+P, Alt+F1)",
  "save": "설정 저장",
  "close_menu": "메뉴 닫기 (Esc)",
  "saved": "PBZFM.ini에 저장했습니다",
  "search": "검색",
  "search_hint": "입력하여 모드 필터링...",
  "theme": "테마",
  "theme_builtin": "먹과 금",
  "select_hint": "왼쪽에서 카테고리를 선택하세요"
})json";

static std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n);
    return w;
}

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// Flat "key": "value" object parser (good enough for the shipped tables).
static bool ParseFlatJson(const std::string& utf8, std::map<std::string, std::string>& out) {
    size_t i = 0;
    const size_t n = utf8.size();
    auto skipWs = [&](size_t& p) { while (p < n && (utf8[p] == ' ' || utf8[p] == '\t' || utf8[p] == '\r' || utf8[p] == '\n')) ++p; };
    auto parseString = [&](size_t& p, std::string& str) -> bool {
        skipWs(p);
        if (p >= n || utf8[p] != '"') return false;
        ++p;
        str.clear();
        while (p < n) {
            char c = utf8[p++];
            if (c == '"') return true;
            if (c == '\\' && p < n) {
                char e = utf8[p++];
                switch (e) {
                    case '"': str += '"'; break;
                    case '\\': str += '\\'; break;
                    case '/': str += '/'; break;
                    case 'n': str += '\n'; break;
                    case 't': str += '\t'; break;
                    case 'r': str += '\r'; break;
                    case 'b': str += '\b'; break;
                    case 'f': str += '\f'; break;
                    case 'u': {
                        if (p + 4 > n) return false;
                        unsigned cp = (unsigned)strtoul(utf8.substr(p, 4).c_str(), nullptr, 16);
                        p += 4;
                        // BMP-only UTF-8 encode; our tables never use surrogates.
                        if (cp < 0x80) str += (char)cp;
                        else if (cp < 0x800) { str += (char)(0xC0 | (cp >> 6)); str += (char)(0x80 | (cp & 0x3F)); }
                        else { str += (char)(0xE0 | (cp >> 12)); str += (char)(0x80 | ((cp >> 6) & 0x3F)); str += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: return false;
                }
            } else str += c;
        }
        return false;
    };
    skipWs(i);
    if (i >= n || utf8[i] != '{') return false;
    ++i;
    while (true) {
        skipWs(i);
        if (i < n && utf8[i] == '}') return true;
        std::string key, val;
        if (!parseString(i, key)) return false;
        skipWs(i);
        if (i >= n || utf8[i] != ':') return false;
        ++i;
        if (!parseString(i, val)) return false;
        out[key] = val;
        skipWs(i);
        if (i < n && utf8[i] == ',') { ++i; continue; }
        if (i < n && utf8[i] == '}') return true;
        return false;
    }
}

static std::string ReadFileText(const std::wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    std::string out;
    char buf[4096];
    size_t rd;
    while ((rd = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, rd);
    fclose(f);
    if (out.size() >= 3 && (unsigned char)out[0] == 0xEF && (unsigned char)out[1] == 0xBB && (unsigned char)out[2] == 0xBF)
        out.erase(0, 3);
    return out;
}

static bool LoadTable(const std::wstring& path, const char* embedded, std::map<std::string, std::string>& out) {
    std::string text = ReadFileText(path);
    if (text.empty() && embedded) text = embedded;
    if (text.empty()) return false;
    std::map<std::string, std::string> parsed;
    if (!ParseFlatJson(text, parsed) && embedded) {
        if (!ParseFlatJson(embedded, parsed)) return false;
    }
    out.swap(parsed);
    return !out.empty();
}

// Embedded copy per shipped language: the dropdown languages keep working
// even without the languages dir; unknown languages fall back to zh-CN.
static const char* EmbeddedTable(const std::string& lang) {
    if (lang == "en-US") return kJsonEn;
    if (lang == "ja-JP") return kJsonJp;
    if (lang == "ko-KR") return kJsonKr;
    return kJsonZh;
}

bool I18n::Resolve(const std::string& lang) {
    std::map<std::string, std::string> table;
    if (!LoadTable(languagesDir_ + L"\\" + Utf8ToWide(lang) + L".json", EmbeddedTable(lang), table)) return false;
    strings_.swap(table);
    current_ = lang;
    return true;
}

bool I18n::Load(const std::wstring& languagesDir, const std::wstring& lang) {
    languagesDir_ = languagesDir;
    LoadTable(languagesDir + L"\\en-US.json", kJsonEn, fallback_);
    std::string want = WideToUtf8(lang);
    if (!Resolve(want)) {
        LoadTable(languagesDir + L"\\zh-CN.json", kJsonZh, strings_);
        current_ = "zh-CN";
    }
    Log("[i18n] language=%s, entries=%zu (en fallback=%zu)",
        current_.c_str(), strings_.size(), fallback_.size());
    return true;
}

bool I18n::SetLanguage(const char* utf8Lang) { return Resolve(utf8Lang); }

const char* I18n::T(const char* key) const {
    auto it = strings_.find(key);
    if (it != strings_.end()) return it->second.c_str();
    auto fb = fallback_.find(key);
    if (fb != fallback_.end()) return fb->second.c_str();
    return key;
}

// Pure static by design (no i18n context needed) - keep in sync with the
// embedded tables above and assets/languages/*.json.
const char* const* I18n::KnownLanguages(int* count) {
    static const char* const langs[] = { "zh-CN", "en-US", "ja-JP", "ko-KR" };
    if (count) *count = 4;
    return langs;
}

} // namespace pbzfm
