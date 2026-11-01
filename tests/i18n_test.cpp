#include "../src/i18n.cpp"
#include <cstdio>
#include <cstring>
#include <cstdarg>
namespace pbzfm { void Log(const char*, ...) {} }

int main() {
    using namespace pbzfm;
    std::map<std::string, std::string> m;
    bool ok = ParseFlatJson(kJsonEn, m);
    printf("ParseFlatJson(kJsonEn) ok=%d size=%zu strlen=%zu\n", (int)ok, m.size(), strlen(kJsonEn));
    printf("head=[%.80s]\n", kJsonEn);
    for (auto& kv : m) printf("  %s = %s\n", kv.first.c_str(), kv.second.c_str());
    I18n i;
    i.Load(L"no-such-dir", L"zh-CN");
    printf("T(title) = %s\n", i.T("title"));
    printf("T(fov)   = %s\n", i.T("fov"));
    return 0;
}