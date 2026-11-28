#include "OrbitCamera.h"
#include "PlusConfig.h"
#include <mc2hook/mc2hook.h>
#include <age/vector/matrix34.h>
#include <age/data/timemgr.h>
#include <Windows.h>
#include <Xinput.h>
#include <cmath>
#include <cstdint>

static_assert(sizeof(void*) == 4, "OrbitCamera requires Win32");
namespace {
    // Native addresses and x86 layouts from the archived Community SDK.
    constexpr uintptr_t kGameState = 0x6C3890;
    constexpr uintptr_t kPlayerManager = 0x69585C;
    constexpr uintptr_t kNetworkMode = 0x6C3421;
    constexpr uintptr_t kSetCamera = 0x5EE900;
    constexpr uintptr_t kSetWorld = 0x5EDB20;
    bool blocked = false;

    template<class T> bool Read(uintptr_t address, T& value) {
        SIZE_T bytes = 0;
        return address && ReadProcessMemory(GetCurrentProcess(),
            reinterpret_cast<const void*>(address), &value, sizeof(T), &bytes)
            && bytes == sizeof(T);
    }
    bool GameplayAllowed() {
        uintptr_t state = 0;
        int mode = 0;
        unsigned char localPause = 0, pause = 0, network = 0;
        return Read(kNetworkMode, network) && !network &&
            Read(kGameState, state) && state && Read(state + 4, mode) && mode == 2 &&
            Read(state + 8, localPause) && !localPause &&
            Read(state + 9, pause) && !pause;
    }
    bool ReadPlayer(uintptr_t& sim, Vector3& pivot, Vector3& velocity) {
        uintptr_t manager = 0, player = 0, entity = 0, collider = 0, ics = 0;
        int count = 0;
        Matrix34 transform;
        // mcPlayerManager::m_Players, m_NumPlayers; mcPlayer::m_Entity;
        // vehEntity::m_Car + mcCar::m_CarSim; vehSim::m_Collider;
        // phCollider::m_ICS; phInertialCS::m_WorldTransform/m_WorldVelocity.
        if (!Read(kPlayerManager, manager) || !manager ||
            !Read(manager + 0x28, count) || count < 1 || count > 8 ||
            !Read(manager + 0x08, player) || !player ||
            !Read(player + 0x10, entity) || !entity ||
            !Read(entity + 0x48, sim) || !sim ||
            !Read(sim + 0x6C, collider) || !collider ||
            !Read(collider + 0x10, ics) || !ics ||
            !Read(ics + 0x4C, transform) || !Read(ics + 0x7C, velocity)) return false;
        pivot = transform.GetRow(3);
        return std::isfinite(pivot.X) && std::isfinite(pivot.Y) && std::isfinite(pivot.Z) &&
            std::isfinite(velocity.X) && std::isfinite(velocity.Y) && std::isfinite(velocity.Z);
    }
    bool IsNativeCall(uintptr_t address, uintptr_t target) {
        unsigned char opcode = 0;
        int32_t rel = 0;
        return Read(address, opcode) && opcode == 0xE8 && Read(address + 1, rel) &&
            address + 5 + rel == target;
    }
    void NativeCamera(const Matrix34& matrix) { hook::StaticThunk<kSetCamera>::Call<void>(&matrix); }
    void NativeWorld(const Matrix34& matrix) { hook::StaticThunk<kSetWorld>::Call<void>(&matrix); }
    bool orbitEnabled = false;
    float orbitYawSpeed = 180.0f, orbitPitchSpeed = 90.0f;
    float orbitDeadZone = 0.18f, orbitReturnSpeed = 5.0f;
    bool orbitInvertX = false;
    bool orbitInvertY = false;
    float orbitRecenterDelay = 1.5f;
    float orbitMoveSpeed = 0.5f; // metres per second
    float orbitMinElevation = 0.0f; // degrees above the vehicle pivot
    float orbitMovingTime = 0.0f;
    bool orbitMoving = false;
    const void* orbitLastCar = nullptr;
    float orbitYaw = 0.0f, orbitPitch = 0.0f;
    bool orbitActive = false;
    Matrix34 orbitMatrix = Matrix34::I;
    typedef DWORD (WINAPI *OrbitGetState)(DWORD, XINPUT_STATE*);
    OrbitGetState orbitGetState = nullptr;

    float OrbitClamp(float x, float lo, float hi) {
        return x < lo ? lo : (x > hi ? hi : x);
    }
    void ResetOrbit() {
        orbitYaw = orbitPitch = 0.0f;
        orbitMovingTime = 0.0f;
        orbitMoving = false;
        orbitLastCar = nullptr;
        orbitActive = false;
    }
    void LoadOrbitInput() {
        const wchar_t* names[] = {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"};
        wchar_t systemDir[MAX_PATH];
        UINT len = GetSystemDirectoryW(systemDir, MAX_PATH);
        if (!len || len >= MAX_PATH - 32) return;
        for (const wchar_t* name : names) {
            wchar_t path[MAX_PATH];
            lstrcpyW(path, systemDir);
            lstrcatW(path, L"\\");
            lstrcatW(path, name);
            HMODULE module = LoadLibraryW(path);
            if (!module) continue;
            orbitGetState = reinterpret_cast<OrbitGetState>(GetProcAddress(module, "XInputGetState"));
            if (orbitGetState) { hook_output("Orbit Camera: XInput ready\n"); return; }
            FreeLibrary(module);
        }
        hook_output("Orbit Camera: XInput unavailable\n");
    }
    float OrbitAxis(SHORT raw) {
        float v = OrbitClamp(raw / 32767.0f, -1.0f, 1.0f);
        float a = fabsf(v);
        if (a <= orbitDeadZone) return 0.0f;
        return (v < 0.0f ? -1.0f : 1.0f) * (a - orbitDeadZone) / (1.0f - orbitDeadZone);
    }
    // Explicit rigid rotation: avoid the SDK's faulty Matrix34::Dot and
    // MakeRotateUnitAxis implementations. Rows are world-space vectors.
    Vector3 RotateVector(const Vector3& v, const Vector3& axis, float angle) {
        const float c = cosf(angle), s = sinf(angle), t = 1.0f - c;
        const float d = axis.X*v.X + axis.Y*v.Y + axis.Z*v.Z;
        return Vector3(
            c*v.X + s*(axis.Y*v.Z-axis.Z*v.Y) + t*d*axis.X,
            c*v.Y + s*(axis.Z*v.X-axis.X*v.Z) + t*d*axis.Y,
            c*v.Z + s*(axis.X*v.Y-axis.Y*v.X) + t*d*axis.Z);
    }
    void UpdateOrbit(const Matrix34& original) {
        orbitActive = false;
        static bool inputAttempted = false;
        if (orbitEnabled && !inputAttempted) { inputAttempted = true; LoadOrbitInput(); }
        if (!orbitEnabled || !orbitGetState || blocked || !GameplayAllowed()) { ResetOrbit(); return; }
        uintptr_t sim = 0;
        Vector3 pivot, velocity;
        if (!ReadPlayer(sim, pivot, velocity)) { ResetOrbit(); return; }
        const void* car = reinterpret_cast<const void*>(sim);
        if (car != orbitLastCar) { ResetOrbit(); orbitLastCar = car; }
        pivot.Y += 0.75f;
        Vector3 offset = original.GetRow(3) - pivot;
        float distanceSq = offset.Mag2();
        // Skip cockpit, distant cinematics and invalid transforms.
        if (!(distanceSq > 2.25f && distanceSq < 625.0f)) { ResetOrbit(); return; }
        XINPUT_STATE input = {};
        bool found = false;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (orbitGetState(i, &input) == ERROR_SUCCESS) { found = true; break; }
        }
        float x = found ? OrbitAxis(input.Gamepad.sThumbRX) : 0.0f;
        float y = found ? OrbitAxis(input.Gamepad.sThumbRY) : 0.0f;
        const float rad = 0.01745329252f;
        float dt = OrbitClamp(datTimeManager::GetActualSeconds(), 0.0f, 0.05f);
        // Require sustained horizontal movement. Hysteresis filters stationary jitter.
        float speedSq = velocity.X * velocity.X + velocity.Z * velocity.Z;
        float threshold = orbitMoving ? orbitMoveSpeed * 0.6f : orbitMoveSpeed;
        orbitMoving = speedSq >= threshold * threshold;
        if (!orbitMoving) orbitMovingTime = 0.0f;
        else orbitMovingTime = OrbitClamp(orbitMovingTime + dt, 0.0f, orbitRecenterDelay);

        if (x != 0.0f || y != 0.0f) {
            orbitYaw = remainderf(orbitYaw - x * (orbitInvertX ? -1.0f : 1.0f) * orbitYawSpeed * rad * dt, 6.28318530718f);
            orbitPitch -= y * (orbitInvertY ? -1.0f : 1.0f) * orbitPitchSpeed * rad * dt;
        } else if (orbitMoving && orbitMovingTime >= orbitRecenterDelay) {
            float decay = expf(-orbitReturnSpeed * dt);
            orbitYaw *= decay; orbitPitch *= decay;
        }
        if (fabsf(orbitYaw) < 0.001f && fabsf(orbitPitch) < 0.001f) {
            orbitYaw = orbitPitch = 0.0f;
            return;
        }
        // Positive pitch lowers the radial vector around axis (Z,0,-X).
        // Bound absolute elevation, not just the relative stick angle.
        // This is a vehicle-relative floor, not a terrain collision query.
        float horizontal = sqrtf(offset.X * offset.X + offset.Z * offset.Z);
        float baseElevation = atan2f(offset.Y, horizontal);
        float minElevation = orbitMinElevation * rad;
        float maxElevation = 75.0f * rad;
        orbitPitch = OrbitClamp(orbitPitch, baseElevation - maxElevation,
                               baseElevation - minElevation);
        orbitMatrix = original;
        const Vector3 up(0.0f, 1.0f, 0.0f);
        Vector3 radial = RotateVector(offset, up, orbitYaw);
        Vector3 axis(radial.Z, 0.0f, -radial.X);
        const float axisLength = sqrtf(axis.X*axis.X + axis.Z*axis.Z);
        const bool applyPitch = axisLength > 0.001f;
        if (applyPitch) {
            axis = Vector3(axis.X/axisLength, 0.0f, axis.Z/axisLength);
            radial = RotateVector(radial, axis, orbitPitch);
        }
        for (int row = 0; row < 3; ++row) {
            Vector3 basis = RotateVector(original.GetRow(row), up, orbitYaw);
            if (applyPitch) basis = RotateVector(basis, axis, orbitPitch);
            orbitMatrix.SetRow(row, basis);
        }
        orbitMatrix.SetRow(3, radial + pivot);
        orbitActive = true;
    }
    void __cdecl CameraHook(const Matrix34& original) {
        UpdateOrbit(original);
        NativeCamera(orbitActive ? orbitMatrix : original);
    }
    void __cdecl OtherCameraHook(const Matrix34& original) {
        NativeCamera(orbitActive ? orbitMatrix : original);
    }
    void __cdecl SkyWorldHook(const Matrix34& original) {
        if (!orbitActive) { NativeWorld(original); return; }
        Matrix34 sky = original;
        sky.SetRow(3, orbitMatrix.GetRow(3));
        NativeWorld(sky);
    }
}
void OrbitCamera::Install() {
    static bool installed = false;
    if (installed) return;
        orbitEnabled = PlusConfig::GetBool("OrbitCamera", "EnableOrbitCamera", false);
        orbitYawSpeed = OrbitClamp(PlusConfig::GetFloat("OrbitCamera", "HorizontalSpeed", 180.0f), 10.0f, 720.0f);
        orbitPitchSpeed = OrbitClamp(PlusConfig::GetFloat("OrbitCamera", "VerticalSpeed", 90.0f), 0.0f, 360.0f);
        orbitDeadZone = OrbitClamp(PlusConfig::GetFloat("OrbitCamera", "DeadZone", 0.18f), 0.0f, 0.95f);
        orbitReturnSpeed = OrbitClamp(PlusConfig::GetFloat("OrbitCamera", "ReturnSpeed", 5.0f), 0.1f, 20.0f);
        orbitInvertX = PlusConfig::GetBool("OrbitCamera", "InvertX", false);
        orbitInvertY = PlusConfig::GetBool("OrbitCamera", "InvertY", false);
        orbitRecenterDelay = OrbitClamp(PlusConfig::GetFloat("OrbitCamera", "RecenterDelay", 1.5f), 0.0f, 10.0f);
        orbitMoveSpeed = OrbitClamp(PlusConfig::GetFloat("OrbitCamera", "MoveSpeedThreshold", 0.5f), 0.1f, 10.0f);
        orbitMinElevation = OrbitClamp(PlusConfig::GetFloat("OrbitCamera", "MinElevation", -10.0f), -75.0f, 30.0f);
    blocked = HookConfig::GetBool("Graphics", "MirrorMode", false) ||
        HookConfig::GetBool("General", "SpeedrunMode", false);
    if (!orbitEnabled || blocked) return;
    // Refuse to overwrite unknown instructions or another camera hook.
    if (!IsNativeCall(0x517C3F, kSetCamera) || !IsNativeCall(0x57C36A, kSetCamera) ||
        !IsNativeCall(0x520662, kSetWorld) || !IsNativeCall(0x5206C3, kSetWorld)) {
        hook_output("OrbitCamera: incompatible native call sites; not installed");
        return;
    }
    InstallCallback("Orbit Camera", "Main camera", &CameraHook, {cb::call(0x517C3F)});
    InstallCallback("Orbit Camera", "Secondary camera", &OtherCameraHook, {cb::call(0x57C36A)});
    InstallCallback("Orbit Camera", "Sky and stars", &SkyWorldHook, {cb::call(0x520662), cb::call(0x5206C3)});
    installed = true;
}
