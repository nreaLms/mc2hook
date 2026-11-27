#pragma once
#include <framework/HookConfig.h>
#include <cmath>
#include "LiveEffectSettings.h"

// Independent effect; submitted by the existing pre-HUD compositor.
class BoostVignette {
    static inline float opacity = 0.0f;
    static float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
    static float Config(const char* key, float def, float lo, float hi) {
        float v = HookConfig::GetFloat("BoostVignette", key, def);
        return std::isfinite(v) ? Clamp(v, lo, hi) : def;
    }
public:
    static bool Enabled() { return LiveEffectSettings::Get(LiveEffectSettings::BoostVignette); }
    static void Reset() { opacity = 0.0f; }
    static float Update(bool active, float dt, bool hasFov, float fovWeight) {
        if (!Enabled()) { Reset(); return 0.0f; }
        static const float baseIntensity = Config("Intensity", .55f, 0.f, 1.f);
        const float intensity = Clamp(baseIntensity * LiveEffectSettings::Scale(LiveEffectSettings::BoostVignette), 0.f, 1.f);
        // Independent fade only when the native FOV envelope is unavailable.
        const float target = active ? intensity : 0.f;
        opacity += (target-opacity)*(1.f-std::exp(-(active ? 22.f : 7.f)*dt));
        return hasFov ? intensity * Clamp(fovWeight, 0.f, 1.f) : Clamp(opacity, 0.f, 1.f);
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
