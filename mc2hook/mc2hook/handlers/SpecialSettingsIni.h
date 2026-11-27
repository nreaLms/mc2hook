#pragma once
#include <string>
#include <cstddef>

// Edit one value in place in the INI text; preserve unrelated bytes, comments
// and existing line endings. Refuse ambiguous duplicate sections or keys.
namespace SpecialSettingsIni {
inline std::string Trim(const std::string& s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
inline bool Equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}
inline bool Update(std::string& text, const char* key, const char* value, const char* alias = nullptr) {
    if (text.find('\0') != std::string::npos) return false; // no UTF-16/binary files
    const std::string nl = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    bool inside = false;
    int sections = 0, matches = 0, aliases = 0;
    size_t aliasStart = 0, aliasFinish = 0, aliasValueStart = 0, aliasValueFinish = 0;
    size_t insert = text.size(), start = 0, finish = 0;
    size_t pos = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
    while (pos < text.size()) {
        const auto lf = text.find('\n', pos);
        const size_t next = lf == std::string::npos ? text.size() : lf + 1;
        size_t end = lf == std::string::npos ? text.size() : lf;
        if (end > pos && text[end-1] == '\r') --end;
        const auto line = text.substr(pos, end-pos);
        const auto comment = line.find_first_of(";#");
        const auto body = line.substr(0, comment);
        const auto clean = Trim(body);
        if (!clean.empty() && clean.front() == '[' && clean.back() == ']') {
            if (inside) insert = pos;
            inside = Equal(Trim(clean.substr(1,clean.size()-2)), "SpecialSettings");
            if (inside) { ++sections; insert = text.size(); }
        } else if (inside) {
            const auto eq = body.find('=');
            if (eq != std::string::npos && Equal(Trim(body.substr(0,eq)),key)) {
                ++matches;
                size_t a = eq+1, b = body.size();
                while (a < b && (body[a]==' ' || body[a]=='\t')) ++a;
                while (b > a && (body[b-1]==' ' || body[b-1]=='\t')) --b;
                start = pos+a; finish = pos+b;
            } else if (eq != std::string::npos && alias && Equal(Trim(body.substr(0,eq)),alias)) {
                ++aliases;
                size_t a = eq+1, b = body.size();
                while (a < b && (body[a]==' ' || body[a]=='\t')) ++a;
                while (b > a && (body[b-1]==' ' || body[b-1]=='\t')) --b;
                aliasValueStart = pos+a; aliasValueFinish = pos+b;
                size_t k = 0, l = eq;
                while (k < l && (body[k]==' ' || body[k]=='\t')) ++k;
                while (l > k && (body[l-1]==' ' || body[l-1]=='\t')) --l;
                aliasStart = pos+k; aliasFinish = pos+l;
            }
        }
        pos = next;
    }
    if (sections > 1 || matches > 1 || aliases > 1) return false;
    if (matches == 1) text.replace(start, finish-start, value);
    else if (aliases == 1) {
        text.replace(aliasValueStart, aliasValueFinish-aliasValueStart, value);
        text.replace(aliasStart, aliasFinish-aliasStart, key);
    } else {
        std::string addition;
        if (insert && text[insert-1] != '\n') addition += nl;
        if (!sections) addition += "[SpecialSettings]" + nl;
        addition += std::string(key) + "=" + value + nl;
        text.insert(insert, addition);
    }
    return true;
}
}
