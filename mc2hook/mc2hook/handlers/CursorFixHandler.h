#pragma once
#include <windows.h>
#include <framework/HookConfig.h>
#include <age/gfx/pipeline.h>
#include <age/state/gamestate.h>
#include <age/types.h>

// Executed on the game's window thread; no global ShowCursor counter changes.
class CursorFixHandler {
    static inline UINT_PTR s_Timer = 0;
    static inline HWND s_Window = nullptr;
    static inline bool s_Hidden = false;
    static bool Enabled() {
        return HookConfig::GetBool("General", "HideCursor", true);
    }
    static bool InGame(HWND hwnd) {
        if (!gfxPipeline::inWindow.get() || GetForegroundWindow() != hwnd) return false;
        auto* state = mcGameState::Instance.get();
        return state && state->m_CurrentState == GameState::Game;
    }
    static bool OverClient(HWND hwnd) {
        POINT p{}; RECT r{};
        if (!GetCursorPos(&p) || WindowFromPoint(p) != hwnd) return false;
        return ScreenToClient(hwnd, &p) && GetClientRect(hwnd, &r) && PtInRect(&r, p);
    }
    static void Restore(HWND hwnd) {
        if (!s_Hidden) return;
        s_Hidden = false;
        // Never override the cursor chosen by a different window.
        if (OverClient(hwnd) && GetForegroundWindow() == hwnd) {
            HCURSOR cursor = reinterpret_cast<HCURSOR>(GetClassLongPtr(hwnd, GCLP_HCURSOR));
            SetCursor(cursor ? cursor : LoadCursor(nullptr, IDC_ARROW));
        }
    }
    static void Refresh(HWND hwnd) {
        if (InGame(hwnd) && OverClient(hwnd)) {
            SetCursor(nullptr);
            s_Hidden = true;
        } else Restore(hwnd);
    }
public:
    static bool BeforeMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT& result) {
        if (hwnd != s_Window || !s_Timer) return false;
        if (msg == WM_SETCURSOR) {
            if (LOWORD(lp) == HTCLIENT && InGame(hwnd)) {
                SetCursor(nullptr); s_Hidden = true; result = TRUE; return true;
            }
            Restore(hwnd);
        } else if (msg == WM_TIMER && wp == s_Timer) {
            Refresh(hwnd); result = 0; return true;
        } else if (msg == WM_DESTROY) {
            Restore(hwnd); KillTimer(hwnd, s_Timer); s_Timer = 0; s_Window = nullptr;
        }
        return false;
    }
    static void AfterMessage(HWND hwnd, UINT msg, LRESULT result) {
        if (msg == WM_CREATE && result != -1 && !s_Window && Enabled()) {
            s_Window = hwnd;
            s_Timer = SetTimer(hwnd, 0x4D433243u, 100, nullptr);
            if (!s_Timer) s_Window = nullptr;
        } else if (hwnd == s_Window && s_Timer &&
                   (msg == WM_ACTIVATE || msg == WM_ACTIVATEAPP || msg == WM_SETFOCUS ||
                    msg == WM_KILLFOCUS || msg == WM_MOUSEMOVE)) {
            Refresh(hwnd);
        }
    }
};
