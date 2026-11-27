#pragma once
#include <framework/HookConfig.h>
#include <atomic>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include "SpecialSettingsIni.h"

// Persistent presets; advanced INI calibration is never rewritten.
class LiveEffectSettings {
public:
    enum Effect { OrbitCamera, CameraShake, LandingShake, BoostShake, BoostCamera,
                  LightTrails, MotionBlur, BoostBlur, BoostVignette, Count };
    enum Level { Off = 0, Low = 1, Medium = 2, High = 3 };
    static bool MenuEnabled() {
        static const bool enabled = HookConfig::GetBool("SpecialSettings", "EnableSpecialSettings", false);
        return enabled;
    }
    static const char* Key(Effect effect) {
        static const char* const keys[] = {"OrbitCamera", "CameraShake", "LandingShake", "BoostShake", "BoostFOVImpulse", "LightTrails", "MotionBlur", "BoostBlur", "BoostVignette"};
        return keys[static_cast<int>(effect)];
    }
    static const char* LevelName(Effect effect, Level level) {
        if (effect == OrbitCamera) return level == Off ? "Off" : "On";
        static const char* const names[] = {"Off", "Low", "Medium", "High"};
        return names[static_cast<int>(level)];
    }
    static Level InitialLevel(Effect effect, bool legacyDefault) {
        const auto key = Key(effect);
        const char* raw = HookConfig::GetString("SpecialSettings", key, nullptr);
        if (!raw && effect == BoostCamera)
            raw = HookConfig::GetString("SpecialSettings", "BoostCamera", nullptr);
        if (raw) {
            const auto v = SpecialSettingsIni::Trim(raw);
            using SpecialSettingsIni::Equal;
            if (Equal(v,"Off") || v == "0") return Off;
            if (effect == OrbitCamera) {
                if (Equal(v,"On") || Equal(v,"Medium") || v == "1" || v == "2") return Medium;
            } else {
                if (Equal(v,"Low") || v == "1") return Low;
                if (Equal(v,"Medium") || Equal(v,"On") || v == "2") return Medium;
                if (Equal(v,"High") || v == "3") return High;
            }
        }
        const char* section = effect == BoostCamera ? "BoostCamera" : key;
        const std::string legacy = std::string("Enable") + section;
        return HookConfig::GetBool(section, legacy.c_str(), legacyDefault) ? Medium : Off;
    }
    static const std::filesystem::path& IniPath() {
        static const auto path = []() -> std::filesystem::path {
            wchar_t buffer[32768];
            const DWORD n = GetFullPathNameW(L"mc2hook.ini", 32768, buffer, nullptr);
            return n && n < 32768 ? std::filesystem::path(buffer) : std::filesystem::path();
        }();
        return path;
    }
    static bool SaveLevel(Effect effect) {
        static std::mutex mutex;
        const std::lock_guard<std::mutex> lock(mutex);
        try {
            const auto& path = IniPath();
            if (path.empty()) return false;
            std::ifstream in(path, std::ios::binary);
            if (!in) return false; // never overwrite an unreadable/missing config
            std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            if (in.bad()) return false;
            in.close();
            if (!SpecialSettingsIni::Update(text, Key(effect), LevelName(effect, GetLevel(effect)),
                effect == BoostCamera ? "BoostCamera" : nullptr)) return false;
            auto temp = path;
            temp += L".specialsettings." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.close();
            if (!out || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(temp.c_str());
                return false;
            }
            return true;
        } catch (...) { return false; }
    }
    static std::atomic<int>& Flag(Effect effect) {
        (void)IniPath(); // capture the startup path before entering frontend modes
        static std::atomic<int> orbit{InitialLevel(OrbitCamera, false)};
        static std::atomic<int> shake{InitialLevel(CameraShake, false)};
        static std::atomic<int> landing{InitialLevel(LandingShake, false)};
        static std::atomic<int> boostShake{InitialLevel(BoostShake, false)};
        static std::atomic<int> boostCamera{InitialLevel(BoostCamera, false)};
        static std::atomic<int> trails{InitialLevel(LightTrails, false)};
        static std::atomic<int> blur{InitialLevel(MotionBlur, false)};
        static std::atomic<int> boostBlur{InitialLevel(BoostBlur, true)};
        static std::atomic<int> vignette{InitialLevel(BoostVignette, false)};
        static std::atomic<int>* const flags[] = {&orbit,&shake,&landing,&boostShake,&boostCamera,&trails,&blur,&boostBlur,&vignette};
        return *flags[static_cast<int>(effect)];
    }
    static Level GetLevel(Effect effect) { return static_cast<Level>(Flag(effect).load()); }
    static bool Get(Effect effect) { return GetLevel(effect) != Off; }
    static bool SetLevel(Effect effect, Level level) {
        if (level < Off || level > High) return false;
        if (effect == OrbitCamera && level != Off) level = Medium;
        return Flag(effect).exchange(static_cast<int>(level)) != static_cast<int>(level);
    }
    static float Scale(Effect effect) {
        switch (GetLevel(effect)) {
        case Low: return 0.5f;
        case Medium: return 1.0f;
        case High: return 1.5f;
        default: return 0.0f;
        }
    }
};
