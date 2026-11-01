#pragma once
// Minimal flat-JSON reader shared by the schema stores and the theme files:
// {"key": "string" | 123 | 1.5 | true, ...} at the top level only. String
// values are unescaped (\u included); every other value is kept as its raw
// token text and coerced by the caller. (i18n.cpp keeps its own copy because
// tests/i18n_test.cpp calls it directly.)
#include <cstdlib>
#include <map>
#include <string>

namespace pbzfm {

inline bool ParseFlatJsonRaw(const std::string& utf8, std::map<std::string, std::string>& out) {
    size_t i = 0;
    const size_t n = utf8.size();
    auto skipWs = [&](size_t& p) {
        while (p < n && (utf8[p] == ' ' || utf8[p] == '\t' || utf8[p] == '\r' || utf8[p] == '\n')) ++p;
    };
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
                        // BMP-only UTF-8 encode; the shipped tables never use surrogates.
                        if (cp < 0x80) str += (char)cp;
                        else if (cp < 0x800) { str += (char)(0xC0 | (cp >> 6)); str += (char)(0x80 | (cp & 0x3F)); }
                        else { str += (char)(0xE0 | (cp >> 12)); str += (char)(0x80 | ((cp >> 6) & 0x3F)); str += (char)(0x80 | (cp & 0x3F)); }
                        break;
                    }
                    default: return false;
                }
            } else {
                str += c;
            }
        }
        return false;
    };
    auto parseRawValue = [&](size_t& p, std::string& str) -> bool {
        skipWs(p);
        size_t start = p;
        while (p < n && utf8[p] != ',' && utf8[p] != '}') ++p;
        str = utf8.substr(start, p - start);
        size_t a = str.find_first_not_of(" \t\r\n");
        size_t b = str.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return false;
        str = str.substr(a, b - a + 1);
        return true;
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
        skipWs(i);
        if (i < n && utf8[i] == '"') {
            if (!parseString(i, val)) return false;
        } else if (!parseRawValue(i, val)) {
            return false;
        }
        out[key] = val;
        skipWs(i);
        if (i < n && utf8[i] == ',') { ++i; continue; }
        if (i < n && utf8[i] == '}') return true;
        return false;
    }
}

} // namespace pbzfm
