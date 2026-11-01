#pragma once
#include <string>
#include <map>

namespace pbzfm {

// Minimal flat-JSON string table ("key": "value"), stored as UTF-8 because
// ImGui takes UTF-8 (char*) directly. Embedded defaults ship in the binary;
// files in <languages dir>/<lang>.json override them.
class I18n {
public:
    bool Load(const std::wstring& languagesDir, const std::wstring& lang);
    bool SetLanguage(const char* utf8Lang);              // one of KnownLanguages()
    const char* T(const char* key) const;                // missing -> en-US -> key itself
    const char* Current() const { return current_.c_str(); }

    static const char* const* KnownLanguages(int* count);

private:
    bool Resolve(const std::string& utf8Lang);
    std::wstring languagesDir_;
    std::string current_ = "zh-CN";
    std::map<std::string, std::string> strings_;         // current language
    std::map<std::string, std::string> fallback_;        // en-US
};

} // namespace pbzfm
