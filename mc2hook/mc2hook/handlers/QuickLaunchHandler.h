#pragma once
#include <mc2hook/mc2hook.h>
#include <framework/HookConfig.h>
#include <age/data/args.h>
#include <windows.h>
#include <cstdint>
#include <cstring>

class QuickLaunchHandler {
    // Called immediately after native device-table initialization (0x467C70).
    // The player input constructor subsequently binds the selected joystick.
    static void __cdecl LoadInitialMapping() {
        using LoadMapping = void (__cdecl*)();
        using SelectDevice = void (__cdecl*)(int, int);
        const auto original = reinterpret_cast<LoadMapping>(0x468320);
        if (!datArgParser::Get("nofe") || datArgParser::Get("nojoy")) {
            original(); return;
        }
        const int preferred = HookConfig::GetInt("QuickLaunch", "ControllerIndex", 0);
        const int count = *reinterpret_cast<const int*>(0x85FBF8);
        // Native table has six entries: keyboard, mouse, four joystick slots.
        if (count <= 0 || count > 4 || preferred < 0 || preferred >= 4) {
            hook_output("[QuickLaunch] No eligible controller (count=%d index=%d); native mapping retained.", count, preferred);
            original(); return;
        }
        int chosen = -1;
        for (int i = 0; i < count; ++i) {
            const int slot = (preferred + i) % count;
            const uintptr_t entry = 0x695AB8u + (2u + slot) * 0x21Cu;
            if (*reinterpret_cast<const unsigned char*>(entry + 8) &&
                *reinterpret_cast<const int*>(entry) == 2 &&
                *reinterpret_cast<const int*>(entry + 4) == slot &&
                *reinterpret_cast<const uintptr_t*>(0x85FC00u + slot * 0x94u + 0x90u)) {
                chosen = slot; break;
            }
        }
        if (chosen < 0) {
            hook_output("[QuickLaunch] No enabled gamepad entry; native mapping retained.");
            original(); return;
        }
        // Same native selector as Pause Options: stores type/index and loads mapping.
        reinterpret_cast<SelectDevice>(0x468520)(2, chosen);
        hook_output("[QuickLaunch] Selected native gamepad %d (type=2), preferred=%d.", chosen, preferred);
    }
public:
    static void Install() {
        if (!HookConfig::GetBool("QuickLaunch", "AutoSelectController", true)) return;
        if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x400000u) return;
        const unsigned char expected[] = {0xE8,0x9F,0xFE,0xFF,0xFF};
        if (std::memcmp(reinterpret_cast<const void*>(0x46847C), expected, sizeof(expected)) != 0) {
            hook_output("[QuickLaunch] Unsupported executable/call site; skipped."); return;
        }
        InstallCallback("QuickLaunchHandler::LoadInitialMapping", "Select controller for nofe startup", &LoadInitialMapping, {cb::call(0x46847C)});
    }
};
