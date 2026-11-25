#pragma once
#include <mc2hook/mc2hook.h>
#include <framework/HookConfig.h>
#include <windows.h>
#include <cstring>
#include <cstdint>

class TrafficDiagnosticsHandler {
    // This exact call site passes two cdecl arguments; return value is unused.
    static void __cdecl ReportUnavailable(const char* format, const char* name) {
        (void)format;
        static ULONGLONG start = GetTickCount64();
        static unsigned count = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - start >= 1000) {
            if (count) hook_output("[Traffic] Unavailable candidate checks: %u in %llu ms (last window).", count, static_cast<unsigned long long>(now-start));
            count = 0;
            start = now;
        }
        if (count == 0) hook_output("[Traffic] Unavailable candidate: %s", name ? name : "(null)");
        ++count;
    }
public:
    static void Install() {
        if (!HookConfig::GetBool("Traffic", "LimitUnavailableLogs", false)) return;
        if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr)) != 0x400000u) return;
        const unsigned char expected[] = {0xE8,0xE7,0x0F,0x13,0x00};
        if (std::memcmp(reinterpret_cast<const void*>(0x4E7664), expected, sizeof(expected)) != 0) {
            hook_output("[Traffic] Unsupported diagnostic call site; skipped."); return;
        }
        InstallCallback("TrafficDiagnosticsHandler::ReportUnavailable", "Rate-limit unavailable traffic candidate logs", &ReportUnavailable, {cb::call(0x4E7664)});
        hook_output("[Traffic] Unavailable candidate log limiter installed.");
    }
};
