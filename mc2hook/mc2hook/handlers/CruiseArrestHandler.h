#pragma once
#include <mc2hook/mc2hook.h>
#include <framework/HookConfig.h>
#include <windows.h>
#include <cstdint>
#include <cstring>

// Experimental: observe the same race record flag used by the native
// checkpoint arrest result. Never synthesize an arrest from damage alone.
static int __cdecl CruiseArrestHideContinueBridge();
static __declspec(naked) void CruiseArrestContinueMenuGate() {
        __asm {
            pushfd
            pushad
            call CruiseArrestHideContinueBridge
            test eax, eax
            jnz omitContinue
            popad
            popfd
            // Replay the complete overwritten seven-byte instruction.
            mov eax, dword ptr [esp + 1D8h]
            push 480A53h
            ret
        omitContinue:
            popad
            popfd
            // Native skip branch: reload zero row count and build Restart.
            push 480B30h
            ret
        }
    }

class CruiseArrestHandler {
    template<class T> static T& At(uintptr_t address) { return *reinterpret_cast<T*>(address); }
    static uintptr_t Ptr(uintptr_t address) { return At<uintptr_t>(address); }
    static inline bool autoPause = true;
    static inline bool diagnostic = true;
    static inline uintptr_t previousRace = 0;
    static inline int previousFlag = -1, previousDamage = -1;
    static inline bool presented = false, pauseAttempted = false;
    static inline bool syntheticPause = false;
    static inline DWORD shownAt = 0;
    static void Clear() {
        previousRace = 0; previousFlag = previousDamage = -1;
        presented = pauseAttempted = syntheticPause = false; shownAt = 0;
    }
    static bool ShowArrested(uintptr_t player) {
        const auto ui = Ptr(player + 0x50);
        if (!ui) return false;
        const auto hud = Ptr(ui + 0x3c);
        if (!hud || !At<unsigned char>(hud + 0x70) || !Ptr(hud + 0x0c + 9 * 4)) return false;
        using Lookup = uintptr_t (__thiscall*)(void*, const char*);
        const auto entry = reinterpret_cast<Lookup>(0x5e17f0)(reinterpret_cast<void*>(0x8582c8), "arrested");
        if (!entry || !Ptr(entry + 8)) return false;
        using SetText = void (__thiscall*)(void*, int, const void*);
        using Show = void (__thiscall*)(void*, int);
        reinterpret_cast<SetText>(0x49d460)(reinterpret_cast<void*>(hud), 9, reinterpret_cast<void*>(Ptr(entry + 8)));
        reinterpret_cast<Show>(0x49c470)(reinterpret_cast<void*>(hud), 9);
        return true;
    }
    static void Tick() {
        const auto game = Ptr(0x6c3890), raceState = Ptr(0x6c38a4);
        if (!game || !raceState || At<int>(game + 4) != 2 ||
            At<unsigned char>(0x6c3421) || At<int>(0x6c32e0) != 0 ||
            At<int>(0x6c3304) != 1) { Clear(); return; }
        const auto manager = Ptr(0x697b88);
        const auto race = manager ? Ptr(manager + 4) : 0;
        if (!race || At<int>(race + 8) != 0 || At<int>(raceState + 4) != 4) {
            Clear(); return;
        }
        if (race != previousRace) { Clear(); previousRace = race; }
        if (At<unsigned char>(game + 8) || At<unsigned char>(game + 9)) return;
        const auto players = Ptr(0x69585c);
        const auto player = players ? Ptr(players + 8) : 0;
        const auto entity = player ? Ptr(player + 0x10) : 0;
        const auto input = entity ? Ptr(entity + 0x44) : 0;
        const auto sim = entity ? Ptr(entity + 0x48) : 0;
        if (!player || !input || !sim) return;
        const int flag = At<unsigned char>(race + 0x8d);
        const int damage = At<int>(sim + 0x1b8);
        if (flag != previousFlag || damage != previousDamage) {
            if (diagnostic) hook_output("[CruiseArrest] recordFlag=%d vehicleState=%d gas=%.3f steer=%.3f", flag, damage, At<float>(input+0x10), At<float>(input+0x1c));
            previousFlag = flag; previousDamage = damage;
        }
        if (flag != 1) { presented = pauseAttempted = false; return; }
        if (!presented) {
            if (!ShowArrested(player)) return;
            presented = true; shownAt = GetTickCount();
            hook_output("[CruiseArrest] Native arrest flag observed; Arrested displayed.");
        }
    }
    // Only the native pause poll can consume this one-shot request.
    static bool ReadyForPause() {
        if (!autoPause || !presented || pauseAttempted ||
            DWORD(GetTickCount() - shownAt) < 1500u) return false;
        const auto game = Ptr(0x6c3890), rs = Ptr(0x6c38a4);
        const auto manager = Ptr(0x697b88), race = manager ? Ptr(manager + 4) : 0;
        const auto menu = Ptr(0x697b90), session = Ptr(0x6c38b4);
        if (!game || !rs || !race || race != previousRace || !menu || !session ||
            !Ptr(menu + 0x4c) || !Ptr(menu + 0x54)) return false;
        return At<int>(game + 4) == 2 && !At<unsigned char>(game + 8) &&
            !At<unsigned char>(game + 9) && !At<unsigned char>(0x6c3421) &&
            At<int>(0x6c32e0) == 0 && At<int>(0x6c3304) == 1 &&
            At<int>(race + 8) == 0 && At<int>(rs + 4) == 4 &&
            At<unsigned char>(race + 0x8d) == 1 && At<int>(session + 8) < 0 &&
            !At<unsigned char>(0x692e78);
    }
    static int __fastcall PauseAction(void* self, void*, int action) {
        using Original = int (__thiscall*)(void*, int);
        const int pressed = reinterpret_cast<Original>(0x467e90)(self, action);
        if (action == 0x18 && ReadyForPause()) {
            syntheticPause = true;
            pauseAttempted = true;
            hook_output("[CruiseArrest] Injecting one native Start action.");
            return 1;
        }
        return pressed;
    }
    static int __fastcall PauseDamageGate(void* self, void*, int index) {
        using Original = int (__thiscall*)(void*, int);
        const int state = reinterpret_cast<Original>(0x4670d0)(self, index);
        // Bypass only the local pause eligibility check. Never write vehicle state.
        if (syntheticPause && index == 0) {
            hook_output("[CruiseArrest] Pause eligibility override (native vehicleState=%d).", state);
            return 0;
        }
        return state;
    }
    static void __fastcall NativePausePoll(void* self, void*) {
        syntheticPause = false;
        const bool wasAttempted = pauseAttempted;
        using Original = void (__thiscall*)(void*);
        reinterpret_cast<Original>(0x47c3c0)(self);
        syntheticPause = false;
        if (!wasAttempted && pauseAttempted) {
            hook_output("[CruiseArrest] Native Start poll returned; pause event expected.");
        }
    }
    static void __fastcall AfterNativeUpdate(void* self, void*) {
        using Original = void (__thiscall*)(void*);
        reinterpret_cast<Original>(0x4aa010)(self);
        Tick();
    }
    // The native offline pause builder has a branch that skips Continue
    // entirely (including its row count increment). Reuse it for arrests.
public:
    static int __cdecl HideContinueForArrest() {
        const auto game = Ptr(0x6c3890), rs = Ptr(0x6c38a4);
        const auto manager = Ptr(0x697b88), race = manager ? Ptr(manager + 4) : 0;
        const auto session = Ptr(0x6c38b4);
        if (!game || !rs || !race || !session || At<int>(session + 8) >= 0 ||
            At<int>(game + 4) != 2 || At<unsigned char>(0x6c3421) ||
            At<int>(0x6c32e0) != 0 || At<int>(0x6c3304) != 1 ||
            At<int>(race + 8) != 0 || At<int>(rs + 4) != 4 ||
            At<unsigned char>(race + 0x8d) != 1) return 0;
        if (diagnostic) hook_output("[CruiseArrest] Continue Cruise omitted from arrest pause menu.");
        return 1;
    }

public:
    static void Install() {
        if (!HookConfig::GetBool("CruisePolice", "EnableArrestRestart", true)) return;
        if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x400000u) return;
        const unsigned char expected[] = {0xe8,0xbc,0x83,0x0a,0x00};
        if (std::memcmp(reinterpret_cast<const void*>(0x401c4f), expected, sizeof(expected))) {
            hook_output("[CruiseArrest] Unsupported or already patched call site; skipped."); return;
        }
        const unsigned char actionSite[] = {0xe8,0x2a,0xba,0xfe,0xff};
        const unsigned char gateSite[] = {0xe8,0x56,0xac,0xfe,0xff};
        const unsigned char pollSite[] = {0xe8,0xf1,0xfb,0xff,0xff};
        if (std::memcmp(reinterpret_cast<const void*>(0x47c461), actionSite, 5) ||
            std::memcmp(reinterpret_cast<const void*>(0x47c475), gateSite, 5) ||
            std::memcmp(reinterpret_cast<const void*>(0x47c7ca), pollSite, 5)) {
            hook_output("[CruiseArrest] Unsupported or already patched pause sites; skipped."); return;
        }
        autoPause = HookConfig::GetBool("CruisePolice", "AutoPauseOnArrest", true);
        diagnostic = HookConfig::GetBool("CruisePolice", "ArrestDiagnostics", true);
        InstallCallback("CruiseArrestHandler::AfterNativeUpdate", "Observe native Cruise arrest record", &AfterNativeUpdate, {cb::call(0x401c4f)});
        InstallCallback("CruiseArrestHandler::PauseAction", "Inject native Start action once", &PauseAction, {cb::call(0x47c461)});
        InstallCallback("CruiseArrestHandler::PauseDamageGate", "Local pause eligibility only", &PauseDamageGate, {cb::call(0x47c475)});
        InstallCallback("CruiseArrestHandler::NativePausePoll", "Scope synthetic Start to native pause poll", &NativePausePoll, {cb::call(0x47c7ca)});
        // Keep the validated v0.3a hooks active even if this optional site differs.
        const unsigned char continueSite[] = {0x8b,0x84,0x24,0xd8,0x01,0x00,0x00};
        const unsigned char skipSite[] = {0x8b,0x74,0x24,0x14,0x8b,0x94,0x24,0xd8,0x01,0x00,0x00};
        if (!std::memcmp(reinterpret_cast<const void*>(0x480a4c), continueSite, sizeof(continueSite)) &&
            !std::memcmp(reinterpret_cast<const void*>(0x480b30), skipSite, sizeof(skipSite))) {
            InstallCallback("CruiseArrestHandler::ContinueMenuGate", "Omit Continue only for native Cruise arrest", &CruiseArrestContinueMenuGate, {cb::jmp(0x480a4c)});
        } else {
            hook_output("[CruiseArrest] Continue menu site mismatch; keeping original menu.");
        }
        hook_output("[CruiseArrest] v0.3b1 arrest pause menu installed.");
    }
};

static int __cdecl CruiseArrestHideContinueBridge() {
    return CruiseArrestHandler::HideContinueForArrest();
}
