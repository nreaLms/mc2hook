#include "PlusConfig.h"
#include <Windows.h>
#include <string>
#include <cstdlib>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <cctype>

namespace {
    // Resolve beside the game executable, independently of the working directory.
    const std::string& IniPath() {
        static const std::string path = []() -> std::string {
            char buffer[MAX_PATH] = {};
            const DWORD length = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
            if (!length || length >= MAX_PATH) return {};
            std::string exe(buffer, length);
            const auto slash = exe.find_last_of("\\/");
            if (slash == std::string::npos) return {};
            return exe.substr(0, slash + 1) + "mc2plus.ini";
        }();
        return path;
    }

    bool ReadValue(const char* section, const char* key, char (&value)[128]) {
        const auto& path = IniPath();
        if (path.empty()) return false;
        const DWORD length = GetPrivateProfileStringA(section, key, "", value,
            static_cast<DWORD>(sizeof(value)), path.c_str());
        return length > 0 && length < sizeof(value) - 1;
    }
}

bool PlusConfig::GetBool(const char* section, const char* key, bool fallback) {
    char value[128] = {};
    if (!ReadValue(section, key, value)) return fallback;
    if (!std::strcmp(value, "1") || !_stricmp(value, "true") ||
        !_stricmp(value, "yes") || !_stricmp(value, "on")) return true;
    if (!std::strcmp(value, "0") || !_stricmp(value, "false") ||
        !_stricmp(value, "no") || !_stricmp(value, "off")) return false;
    return fallback;
}

float PlusConfig::GetFloat(const char* section, const char* key, float fallback) {
    char value[128] = {};
    if (!ReadValue(section, key, value)) return fallback;
    char* end = nullptr;
    errno = 0;
    const float result = std::strtof(value, &end);
    if (end == value || errno == ERANGE || !std::isfinite(result)) return fallback;
    while (*end && std::isspace(static_cast<unsigned char>(*end))) ++end;
    return *end == '\0' ? result : fallback;
}
