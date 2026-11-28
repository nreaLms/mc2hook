#pragma once

namespace PlusConfig {
    bool GetBool(const char* section, const char* key, bool fallback);
    float GetFloat(const char* section, const char* key, float fallback);
}
