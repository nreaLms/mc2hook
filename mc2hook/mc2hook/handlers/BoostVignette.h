#pragma once
#include <framework/HookConfig.h>
#include <cmath>

// Independent effect; submitted by the existing pre-HUD compositor.
class BoostVignette {
    static inline float opacity = 0.0f, age = 0.0f;
    static inline bool wasActive = false;
    static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float Config(const char* key, float def, float lo, float hi) {
        float v = HookConfig::GetFloat("BoostVignette", key, def);
        return std::isfinite(v) ? Clamp(v, lo, hi) : def;
    }
public:
    static bool Enabled() { static const bool v = HookConfig::GetBool("BoostVignette", "EnableBoostVignette", false); return v; }
    static void Reset() { opacity = age = 0.0f; wasActive = false; }
    static float Update(bool active, float dt) {
        if (!Enabled()) { Reset(); return 0.0f; }
        static const float intensity = Config("Intensity", .35f, 0.f, 1.f);
        static const float kick = Config("KickIntensity", .55f, 0.f, 1.f);
        static const float duration = Config("KickDuration", .30f, .05f, 2.f);
        if (active && !wasActive) age = 0.f;
        float t = Clamp(age / duration, 0.f, 1.f);
        float envelope = (1.f-t)*(1.f-t)*(1.f+2.f*t);
        float target = active ? intensity + ((kick > intensity ? kick : intensity)-intensity)*envelope : 0.f;
        opacity += (target-opacity)*(1.f-std::exp(-(active ? 22.f : 7.f)*dt));
        if (active) age = Clamp(age+dt, 0.f, duration);
        wasActive = active;
        return opacity;
    }
    static float Alpha(float x, float y, float strength) {
        static const float radius = Config("Radius", .55f, 0.f, .95f);
        static const float softness = Config("Softness", .80f, .05f, 1.f);
        float dx = 2.f*x-1.f, dy = 2.f*y-1.f;
        // Ellipse in normalized screen coordinates; alpha reaches full strength at corners.
        float r = std::sqrt(dx*dx+dy*dy)*.70710678f;
        float t = Clamp((r-radius*.70710678f)/((1.f-radius*.70710678f)*softness), 0.f, 1.f);
        return Clamp(strength*t*t*(3.f-2.f*t), 0.f, 1.f);
    }
};
