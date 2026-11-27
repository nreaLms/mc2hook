#pragma once
#include <mc2hook/mc2hook.h>
#include <framework/HookConfig.h>
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include "LiveEffectSettings.h"

// v0.4c: native audio level icons and English menu labels.
// This prototype deliberately uses the native arrow navigation, not a new
// frontend mode or a custom Enter/Escape dispatcher.
class SpecialSettingsHandler {
    using AddOption = void* (__thiscall*)(void*, const char*, const char*, int, void*, int, int);
    using AddWideOption = void* (__thiscall*)(void*, const wchar_t*, const char*, int, void*, int, const char*);
    using GroupCtor = void* (__thiscall*)(void*, int, int, const char*);
    using NativeAlloc = void* (__cdecl*)(unsigned int, const char*, int);
    using RefMethod = void (__thiscall*)(void*);
    static constexpr int SpecialAction = 7; // Native Options dispatcher handles 0..6 only.
    using LinkGroup = void (__thiscall*)(void*, void*);
    static constexpr int VideoRowIds[] = {0x2F, 0x30, 0x31, 0x32, 0x2E, 0x34, 0x35, 0x36, 0x37}; // Existing native options row IDs; reused experimentally.
    static constexpr const char* VideoIcon = "ui_icon_opt_video";
    // Native audio menu formats ui_icon_race%d at 0x42D7A3.
    static constexpr const char* LevelIcons[] = {"ui_icon_race0", "ui_icon_race1", "ui_icon_race2", "ui_icon_race3"};

    static void Retain(void* p) {
        if (p) reinterpret_cast<RefMethod>((*reinterpret_cast<void***>(p))[1])(p);
    }
    static void Release(void* p) {
        if (p) reinterpret_cast<RefMethod>((*reinterpret_cast<void***>(p))[2])(p);
    }
    static void* NewGroup(int layoutId) {
        void* mem = reinterpret_cast<NativeAlloc>(0x5772E0)(0x54, "SpecialSettings", __LINE__);
        if (!mem) return nullptr;
        void* group = reinterpret_cast<GroupCtor>(0x44CFC0)(mem, 300, layoutId, nullptr);
        Retain(group); // Local owning reference; ctor starts at zero.
        return group;
    }
    static void* AddWide(void* group, const wchar_t* label, const char* icon,
                         int value, void* child, int action) {
        // Native AddOption consumes the by-value intrusive reference.
        Retain(child);
        return reinterpret_cast<AddWideOption>(0x44D2D0)(group, label, icon, value, child, action, nullptr);
    }
    static void SetHelp(void* entry, const wchar_t* text) {
        // SetHelpTextFromStringtable stores a borrowed wchar_t* at +0x10.
        // Our text has static lifetime. No game allocation or ownership transfer.
        if (entry) {
            void* option = *reinterpret_cast<void**>(entry);
            if (option) *reinterpret_cast<const wchar_t**>(static_cast<unsigned char*>(option) + 0x10) = text;
        }
    }
    static constexpr int RowCount = LiveEffectSettings::Count;
    // Presentation order only: preserve the effect enum, layout IDs and save mapping.
    static constexpr LiveEffectSettings::Effect DisplayOrder[RowCount] = {
        LiveEffectSettings::OrbitCamera, LiveEffectSettings::CameraShake,
        LiveEffectSettings::LandingShake, LiveEffectSettings::LightTrails,
        LiveEffectSettings::MotionBlur, LiveEffectSettings::BoostCamera,
        LiveEffectSettings::BoostShake, LiveEffectSettings::BoostBlur,
        LiveEffectSettings::BoostVignette
    };
    static constexpr const wchar_t* Labels[RowCount][4] = {
        {L"Orbit Camera: Off", L"Orbit Camera: On", nullptr, nullptr},
        {L"Camera Shake: Off", L"Camera Shake: Low", L"Camera Shake: Medium", L"Camera Shake: High"},
        {L"Landing Shake: Off", L"Landing Shake: Low", L"Landing Shake: Medium", L"Landing Shake: High"},
        {L"Boost Shake: Off", L"Boost Shake: Low", L"Boost Shake: Medium", L"Boost Shake: High"},
        {L"Boost FOV Impulse: Off", L"Boost FOV Impulse: Low", L"Boost FOV Impulse: Medium", L"Boost FOV Impulse: High"},
        {L"Light Trails: Off", L"Light Trails: Low", L"Light Trails: Medium", L"Light Trails: High"},
        {L"Motion Blur: Off", L"Motion Blur: Low", L"Motion Blur: Medium", L"Motion Blur: High"},
        {L"Boost Blur: Off", L"Boost Blur: Low", L"Boost Blur: Medium", L"Boost Blur: High"},
        {L"Boost Vignette: Off", L"Boost Vignette: Low", L"Boost Vignette: Medium", L"Boost Vignette: High"}
    };
    static constexpr const char* Names[RowCount] = {"Orbit Camera", "Camera Shake", "Landing Shake", "Boost Shake", "Boost FOV Impulse", "Light Trails", "Motion Blur", "Boost Blur", "Boost Vignette"};
    static constexpr const char* LevelNames[] = {"Off", "Low", "Medium", "High"};
    static int OptionCount(int row) { return row == 0 ? 2 : 4; }
    static int MenuIndex(int row) {
        const auto level = LiveEffectSettings::GetLevel(static_cast<LiveEffectSettings::Effect>(row));
        return row == 0 ? (level == LiveEffectSettings::Off ? 0 : 1) : static_cast<int>(level);
    }
    // Match every label, count and layout ID, not native row IDs alone.
    // Do not keep raw group pointers across frontend rebuilds.
    static void __fastcall SelectionChanged(void* group, void*) {
        if (group) {
            auto* g = static_cast<unsigned char*>(group);
            const int layout = *reinterpret_cast<int*>(g + 0x0C);
            for (int i = 0; i < RowCount; ++i) {
                if (layout != VideoRowIds[i]) continue;
                const int count = OptionCount(i);
                const auto begin = *reinterpret_cast<uintptr_t*>(g + 0x18);
                const auto end = *reinterpret_cast<uintptr_t*>(g + 0x1C);
                const int index = *reinterpret_cast<int*>(g + 0x10);
                if (!begin || end < begin || end - begin != static_cast<uintptr_t>(count * 0x10) || index < 0 || index >= count) break;
                bool match = true;
                for (int j = 0; j < count; ++j) {
                    auto* option = *reinterpret_cast<unsigned char**>(begin + j * 0x10);
                    if (!option) { match = false; break; }
                    const auto label = *reinterpret_cast<const wchar_t**>(option + 0x0C);
                    if (!label || std::wcscmp(label, Labels[i][j]) != 0) { match = false; break; }
                }
                if (match) {
                    const auto level = static_cast<LiveEffectSettings::Level>(i == 0 && index == 1 ? LiveEffectSettings::Medium : index);
                    const auto effect = static_cast<LiveEffectSettings::Effect>(i);
                    if (LiveEffectSettings::SetLevel(effect, level)) {
                        const bool saved = LiveEffectSettings::SaveLevel(effect);
                        hook_output("[SpecialSettings] %s: %s (%s).", Names[i], LiveEffectSettings::LevelName(effect, level), saved ? "saved" : "save failed; session only");
                    }
                }
                break;
            }
        }
        reinterpret_cast<void (__thiscall*)(void*)>(0x44C0A0)(group);
    }
    static void* BuildSubgroup() {
        static const wchar_t binaryHelp[] = L"Left / Right: Off / On";
        static const wchar_t levelsHelp[] = L"Left / Right: Off / Low / Medium / High";
        void* next = nullptr;
        for (int position = RowCount - 1; position >= 0; --position) {
            const int i = static_cast<int>(DisplayOrder[position]);
            void* row = NewGroup(VideoRowIds[i]);
            if (!row) { Release(next); return nullptr; }
            for (int j = 0; j < OptionCount(i); ++j) {
                void* entry = AddWide(row, Labels[i][j], LevelIcons[j], j, nullptr, -1);
                if (!entry) { Release(row); Release(next); return nullptr; }
                SetHelp(entry, i == 0 ? binaryHelp : levelsHelp);
            }
            *reinterpret_cast<int*>(static_cast<unsigned char*>(row) + 0x10) = MenuIndex(i);
            if (next) {
                Retain(next);
                reinterpret_cast<LinkGroup>(0x44BE40)(row, next);
                Release(next);
            }
            next = row;
        }
        return next;
    }
    static void* __fastcall AddCreditsHook(void* group, void*, const char* key,
        const char* icon, int index, void* target, int eventId, int help) {
        const auto add = reinterpret_cast<AddOption>(0x44D1C0);
        if (group && !target && eventId == 4 && key && std::strcmp(key, "CM_Credits") == 0) {
            void* child = BuildSubgroup();
            if (child) {
                // No duplicate Credits action; distinct ID, child owned by entry.
                static const wchar_t label[] = L"Special Settings";
                static const wchar_t helpText[] = L"Down: settings / Up: back";
                void* entry = AddWide(group, label, VideoIcon, index, child, SpecialAction);
                Release(child);
                SetHelp(entry, helpText);
                hook_output("[SpecialSettings] v0.4c intensity preset rows added.");
            } else {
                hook_output("[SpecialSettings] Could not allocate test groups; original menu retained.");
            }
        }
        // Preserve original Credits and return its native entry, not a stale vector pointer.
        return add(group, key, icon, index, target, eventId, help);
    }
    template <size_t N> static bool Matches(uintptr_t address, const unsigned char (&bytes)[N]) {
        return std::memcmp(reinterpret_cast<const void*>(address), bytes, N) == 0;
    }
public:
    static void Install() {
        if (!LiveEffectSettings::MenuEnabled()) return;
        if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x400000u) return;
        const unsigned char call[] = {0xE8,0x79,0xED,0x01,0x00};
        const unsigned char add[] = {0x83,0xEC,0x64,0x53,0x55,0x56,0x57,0x68};
        const unsigned char wide[] = {0x83,0xEC,0x64,0x53,0x56,0x57,0x68,0x7F,0x02,0x00,0x00};
        const unsigned char ctor[] = {0x56,0x8B,0xF1,0xE8,0xC8,0x90,0x1B,0x00};
        const unsigned char alloc[] = {0xA1,0xD0,0x02,0x86,0x00,0x85,0xC0};
        const unsigned char link[] = {0x56,0x8B,0x74,0x24,0x08,0x57,0x8B,0xF9};
        const unsigned char selectCall[] = {0xE8,0xB4,0xFC,0xFF,0xFF};
        const unsigned char selectFn[] = {0x56,0x8B,0x71,0x48,0x85,0xF6};
        if (!Matches(0x44C3E7,selectCall) || !Matches(0x44C0A0,selectFn) || !Matches(0x44BE40,link) || !Matches(0x42E442,call) || !Matches(0x44D1C0,add) ||
            !Matches(0x44D2D0,wide) || !Matches(0x44CFC0,ctor) || !Matches(0x5772E0,alloc)) {
            hook_output("[SpecialSettings] Unsupported executable or modified code; skipped.");
            return;
        }
        InstallCallback("SpecialSettings", "Live selection change", &SelectionChanged, {cb::call(0x44C3E7)});
        InstallCallback("SpecialSettings", "Native linked rows", &AddCreditsHook, {cb::call(0x42E442)});
        hook_output("[SpecialSettings] v0.4c live native preset list installed.");
    }
};
