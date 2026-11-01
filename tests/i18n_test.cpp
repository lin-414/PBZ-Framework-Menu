#include "../src/i18n.cpp"
#include <cstdio>
#include <cstring>
#include <cstdarg>
#include <filesystem>
#include <fstream>
namespace pbzfm { void Log(const char*, ...) {} }

static int Check(const char* what, const char* got, const char* want) {
    bool ok = strcmp(got, want) == 0;
    printf("%-34s got=\"%s\" want=\"%s\" -> %s\n", what, got, want, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main() {
    using namespace pbzfm;
    int fails = 0;

    std::map<std::string, std::string> m;
    bool ok = ParseFlatJson(kJsonEn, m);
    printf("ParseFlatJson(kJsonEn) ok=%d size=%zu\n", (int)ok, m.size());
    if (!ok) ++fails;

    int langCount = 0;
    I18n::KnownLanguages(&langCount);
    printf("KnownLanguages count=%d\n", langCount);
    if (langCount != 4) ++fails;

    // No languages dir anywhere: every shipped language must come from its
    // embedded table; unknown keys fall through to the key itself.
    I18n zh;
    zh.Load(L"no-such-dir", L"zh-CN");
    fails += Check("zh T(fov)", zh.T("fov"), "视野 (FOV)");
    fails += Check("zh missing key -> key", zh.T("no_such_key"), "no_such_key");

    I18n en;
    en.Load(L"no-such-dir", L"en-US");
    fails += Check("en T(fov)", en.T("fov"), "Field of view (FOV)");

    I18n jp;
    jp.Load(L"no-such-dir", L"ja-JP");
    fails += Check("ja T(page_settings)", jp.T("page_settings"), "設定");
    fails += Check("ja T(close_menu)", jp.T("close_menu"), "メニューを閉じる (Esc)");

    I18n kr;
    kr.Load(L"no-such-dir", L"ko-KR");
    fails += Check("ko T(quality)", kr.T("quality"), "화질 프리셋");
    fails += Check("ko T(save)", kr.T("save"), "설정 저장");

    // Regression: Resolve() used to build the disk path without the .json
    // extension, so disk tables were never read. A table on disk must win
    // over the embedded copy, and missing keys must fall through to en-US.
    {
        std::filesystem::path dir = std::filesystem::absolute("i18n_test_langs");
        std::filesystem::create_directories(dir);
        {
            std::ofstream f(dir / "zh-CN.json", std::ios::binary);
            f << "{\n  \"page_settings\": \"DISK_VALUE\",\n  \"disk_only\": \"from disk\"\n}\n";
        }
        I18n d;
        d.Load(dir.wstring(), L"zh-CN");
        fails += Check("disk override T(page_settings)", d.T("page_settings"), "DISK_VALUE");
        fails += Check("disk-only key T(disk_only)", d.T("disk_only"), "from disk");
        fails += Check("disk gap falls back to en", d.T("fov"), "Field of view (FOV)");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    printf("%s (%d failure(s))\n", fails == 0 ? "ALL OK" : "FAILED", fails);
    return fails;
}
