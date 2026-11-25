#include "FreecamHandler.h"
#include <age/vector/matrix34.h>
#include <age/vector/matrix44.h>
#include <age/gfx/rstate.h>
#include <age/gfx/viewport.h>
#include <age/core/output.h>
#include <age/input/keyboard.h>
#include <age/input/mouse.h>
#include <age/data/timemgr.h>
#include <dinput.h>
#include <Xinput.h>
#include <cmath>
#include <cstdio>
#include <framework/hook_output.h>
#include <age/state/gamestate.h>
#include <age/types.h>
#include <age/mcgame/playermgr.h>
#include <age/mcgame/player.h>
#include <age/vehicle/entity.h>
#include <mccar/carsim.h>
#include <veh_dyna/wheel.h>
#include <age/physics/phcollider.h>
#include <age/physics/phlevel.h>
#include <age/physics/phinertia.h>
#include <age/mcnetwork/netmanager.h>

#include <age/gfx/rstate.h> //
//#include <age/camera/playercamera.h>

// TODO:
// Attach rain particles to the camera and not the car
// Create a FreeCamHandler::Update instead of 2 SetCamera functions
// Make a standard Lerp function
// Add boost mode with Shift
// Add overall smoothing, for movement and rotation
// Make controls FPS independant
// Disable HUD
// Add camera roll, and a way to reset back to 0
// Clean up and split things up into neater functions

// Disable game input and use WASD(QE) instead
// Add a way to increase/decrease fog distance, for nice distant shots

// Init variables
static bool overrideCameraMtx = false;
static bool freecamToggle = false;
Matrix34 customCameraMtx34 = Matrix34::I;
Matrix34 customSkyboxCameraMtx34 = Matrix34::I;
Matrix34 audioMtx = Matrix34::I;

static bool mirrorMode = false;
static float mirrorFactor = 1.0f;

Vector3 dir;
Vector3 e;

float translateXZSpeed, translateYSpeed;
float translateMultiplier = 1.0f; // Initial translate speed multiplier
float translateMultiplierStep, minTranslateMultiplier, maxTranslateMultiplier;

float rotYSensitivity, rotXSensitivity;

float customFOV = 30.0f; // Initial custom FOV
float minFOV, maxFOV, fovStep;

float fovRotSensitivity = 1.0f;

static int currentGameState = 0;

float cameraNearClip, cameraFarClip;

// Init controls
int activateFreecam, freecamLeft, freecamRight, freecamForward, freecamBackward,
freecamUp, freecamDown, freecamIncreaseFOV, freecamDecreaseFOV, activateReplayCam;

// MC2 Orbit Camera v0.3 - experimental, offline only.
namespace {
    bool landingEnabled = false, landingDebug = false;
    float landingIntensity = 0.8f, landingDuration = 0.24f;
    float landingAir = 0.0f, landingDown = 0.0f;
    float landingAge = 0.0f, landingStrength = 0.0f;
    bool landingWasGrounded = false, landingReady = false;
    bool shakeEnabled = false;
    float shakeMinSpeed = 100.0f, shakeMaxSpeed = 280.0f;
    float shakeIntensity = 0.15f, shakeFrequency = 12.0f, shakeSmoothing = 5.0f;
    float shakeTime = 0.0f, shakeAmount = 0.0f;
    const mcCarSim* shakeLastCar = nullptr;
    bool shakeActive = false;
    Matrix34 shakeMatrix = Matrix34::I;
    Matrix34 shakeBaseMatrix = Matrix34::I;
    bool orbitEnabled = false;
    float orbitYawSpeed = 180.0f, orbitPitchSpeed = 90.0f;
    float orbitDeadZone = 0.18f, orbitReturnSpeed = 5.0f;
    bool orbitInvertY = false;
    float orbitRecenterDelay = 1.5f;
    float orbitMoveSpeed = 0.5f; // metres per second
    float orbitMinElevation = 0.0f; // degrees above the vehicle pivot
    bool orbitCollision = true;
    float orbitCollisionRadius = 0.35f;
    float orbitCollisionPadding = 0.10f;
    float orbitCollisionRecovery = 8.0f;
    float orbitSafeDistance = -1.0f;
    float orbitMovingTime = 0.0f;
    bool orbitMoving = false;
    const mcCarSim* orbitLastCar = nullptr;
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
        orbitSafeDistance = -1.0f;
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
            if (orbitGetState) { Printf("Orbit Camera: XInput ready\n"); return; }
            FreeLibrary(module);
        }
        Printf("Orbit Camera: XInput unavailable\n");
    }
    float OrbitAxis(SHORT raw) {
        float v = OrbitClamp(raw / 32767.0f, -1.0f, 1.0f);
        float a = fabsf(v);
        if (a <= orbitDeadZone) return 0.0f;
        return (v < 0.0f ? -1.0f : 1.0f) * (a - orbitDeadZone) / (1.0f - orbitDeadZone);
    }
    // Native chase-camera sweep: mc2.exe 0x57A69B calls phLevel vtable +0xB8.
    // The original caller reads the remaining distance at result +0x20.
    // ABI and result offsets are inferred from the supplied 32-bit executable.
    struct OrbitSweepResult {
        unsigned char prefix[0x04];
        float normalX, normalY, normalZ; // Native caller reads result +0x04..0x0C.
        unsigned char middle[0x10];
        float remaining;
        unsigned char suffix[0x40 - 0x24];
    };
    static_assert(sizeof(OrbitSweepResult) == 0x40, "Sweep buffer size");
    typedef bool (__thiscall *OrbitSweep)(phLevel*, Vector3*, Vector3*, float,
                                         OrbitSweepResult*, int, int, int);
    void ApplyOrbitCollision(const Vector3& pivot, mcCarSim* sim, float dt) {
        if (!orbitCollision) { orbitSafeDistance = -1.0f; return; }
        phLevel* level = phLevel::Instance;
        if (!level || !sim->m_Collider->m_SomeInstParent) {
            orbitSafeDistance = -1.0f; return;
        }
        Vector3 start = pivot;
        Vector3 end = orbitMatrix.GetRow(3);
        Vector3 delta = end - start;
        float distance = delta.Mag();
        if (!(distance > 0.001f && distance < 30.0f)) return;
        // Same player exclusion ID and default mask (0x1F) as camTrackCS.
        int ignoreId = reinterpret_cast<int>(sim->m_Collider->m_SomeInstParent->m_Archetype);
        void** vtable = *reinterpret_cast<void***>(level);
        if (!vtable || !vtable[0xB8 / sizeof(void*)]) return;
        OrbitSweep sweep = reinterpret_cast<OrbitSweep>(vtable[0xB8 / sizeof(void*)]);
        OrbitSweepResult hit = {};
        float allowed = distance;
        if (sweep(level, &start, &end, orbitCollisionRadius, &hit, ignoreId, 0x1F, 1)) {
            // Match camTrackCS at 0x57A6A9: accept only a normal opposing
            // the normalized sweep direction (native threshold -0.00001f).
            const float normalDot = (hit.normalX * delta.X + hit.normalY * delta.Y +
                                     hit.normalZ * delta.Z) / distance;
            // The native caller accepts only a hit fraction strictly between 0 and 1.
            // Reject exit faces and invalid/initial-overlap distances rather than
            // collapsing the camera onto the pivot.
            if (std::isfinite(normalDot) && normalDot < -0.00001f &&
                std::isfinite(hit.remaining) && hit.remaining > 0.0f &&
                hit.remaining < distance) {
                allowed = OrbitClamp(distance - hit.remaining - orbitCollisionPadding, 0.0f, distance);
            }
        }
        if (orbitSafeDistance < 0.0f || allowed < orbitSafeDistance) {
            orbitSafeDistance = allowed; // retract immediately, never interpolate through wall
        } else {
            orbitSafeDistance += (allowed - orbitSafeDistance) * (1.0f - expf(-orbitCollisionRecovery * dt));
        }
        orbitSafeDistance = OrbitClamp(orbitSafeDistance, 0.0f, allowed);
        orbitMatrix.SetRow(3, pivot + delta * (orbitSafeDistance / distance));
    }
    void LandingDiagnostic(const char* stage, mcCarSim* sim = nullptr, float distanceSq = 0.0f, float dt = 0.0f) {
        if (!landingDebug) return;
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (last && now - last < 750) return;
        last = now;
        hook_output("[LandingShake] stage=%s enabled=%d wheels=%d distanceSq=%.2f dt=%.5f", stage, int(landingEnabled), sim ? int(sim->m_NumWheels) : -1, distanceSq, dt);
    }
    void ResetShake() {
        shakeActive = false;
        shakeTime = shakeAmount = 0.0f;
        shakeLastCar = nullptr;
        landingAir = landingDown = landingAge = landingStrength = 0.0f;
        landingWasGrounded = landingReady = false;
    }
    // Require a genuine airborne interval and downward speed before touchdown.
    float UpdateLanding(mcCarSim* sim, float vy, float dt) {
        if (!landingEnabled || !std::isfinite(vy) || sim->m_NumWheels < 1 || sim->m_NumWheels > 4) {
            LandingDiagnostic("invalid landing input", sim, 0.0f, dt);
            landingReady = false;
            landingAir = landingDown = landingStrength = 0.0f;
            return 0.0f;
        }
        bool grounded = false;
        unsigned groundMask = 0;
        for (int i = 0; i < sim->m_NumWheels; ++i) {
            if (!sim->m_Wheels[i]) { LandingDiagnostic("null wheel", sim, 0.0f, dt); landingReady = false; landingAir = landingDown = landingStrength = 0.0f; return 0.0f; }
            if (sim->m_Wheels[i]->m_OnGround) groundMask |= (1u << i);
            grounded = groundMask != 0;
        }
        if (landingDebug) {
            static ULONGLONG last = 0;
            const ULONGLONG now = GetTickCount64();
            if (!last || now-last >= 500 || (landingReady && grounded != landingWasGrounded)) {
                last = now;
                hook_output("[LandingShake] ground=%d mask=0x%X wheels=%d vy=%.2f air=%.3f peakDown=%.2f strength=%.3f remaining=%.3f dt=%.5f", int(grounded), groundMask, int(sim->m_NumWheels), vy, landingAir, landingDown, landingStrength, landingStrength > 0.0f ? OrbitClamp(landingDuration-landingAge,0.0f,landingDuration) : 0.0f, dt);
            }
        }
        if (!landingReady) {
            landingReady = true;
            landingWasGrounded = grounded;
            landingAir = landingDown = landingStrength = 0.0f;
            return 0.0f;
        }
        if (!grounded) {
            if (landingWasGrounded) { landingAir = landingDown = 0.0f; }
            landingAir += dt;
            const float down = OrbitClamp(-vy, 0.0f, 30.0f);
            if (down > landingDown) landingDown = down;
        } else if (!landingWasGrounded) {
            const bool trigger = landingAir >= 0.08f && landingDown >= 1.5f;
            if (landingDebug) hook_output("[LandingShake] touchdown: air=%.3fs peakDown=%.2f m/s trigger=%d", landingAir, landingDown, int(trigger));
            if (trigger) {
                landingStrength = OrbitClamp(landingDown / 8.0f, 0.25f, 1.0f);
                landingAge = 0.0f;
            }
            landingAir = landingDown = 0.0f;
        }
        landingWasGrounded = grounded;
        if (landingStrength <= 0.0f) return 0.0f;
        landingAge += dt;
        const float t = landingAge / landingDuration;
        if (t >= 1.0f) { landingStrength = 0.0f; return 0.0f; }
        const float envelope = 1.0f - t;
        return landingIntensity * landingStrength * envelope * sinf(t * 6.28318530718f) * 0.01745329252f;
    }
    // Rotate a copy of the final camera, leaving its collision-tested position intact.
    void UpdateShake(const Matrix34& original) {
        shakeActive = false;
        if ((!shakeEnabled && !landingEnabled) || overrideCameraMtx || mirrorMode ||
            datTimeManager::SpeedrunMode || mcNetManager::IsNetworkMode) { LandingDiagnostic("mode blocked"); ResetShake(); return; }
        mcGameState* state = mcGameState::Instance;
        if (!state || state->m_CurrentState != GameState::Game || state->m_IsPaused || state->m_IsPausedLocally) { LandingDiagnostic("game state blocked"); ResetShake(); return; }
        mcPlayerManager* manager = mcPlayerManager::Instance;
        if (!manager || manager->m_NumPlayers < 1 || !manager->m_Players[0] || !manager->m_Players[0]->m_Entity) { LandingDiagnostic("player unavailable"); ResetShake(); return; }
        mcCarSim* sim = manager->m_Players[0]->m_Entity->m_Car.m_CarSim;
        if (!sim || !sim->m_Collider || !sim->m_Collider->m_ICS) { LandingDiagnostic("physics unavailable"); ResetShake(); return; }
        if (sim != shakeLastCar) { ResetShake(); shakeLastCar = sim; }
        Vector3 pivot = sim->m_Collider->m_ICS->m_WorldTransform.GetRow(3);
        pivot.Y += 0.75f;
        float distanceSq = (original.GetRow(3) - pivot).Mag2();
        if (!(distanceSq > 2.25f && distanceSq < 625.0f)) { LandingDiagnostic("camera distance rejected", sim, distanceSq); ResetShake(); return; }
        const Vector3& velocity = sim->m_Collider->m_ICS->m_WorldVelocity;
        float speed = sqrtf(velocity.X * velocity.X + velocity.Z * velocity.Z) * 3.6f;
        if (!std::isfinite(speed)) { LandingDiagnostic("invalid speed", sim); ResetShake(); return; }
        float dt = OrbitClamp(datTimeManager::GetActualSeconds(), 0.0f, 0.05f);
        const float landingPitch = UpdateLanding(sim, velocity.Y, dt);
        if (landingDebug && fabsf(landingPitch) > 0.000001f) {
            static ULONGLONG lastPitch = 0;
            const ULONGLONG now = GetTickCount64();
            if (!lastPitch || now-lastPitch >= 100) {
                lastPitch = now;
                hook_output("[LandingShake] applied pitch=%.3f deg remaining=%.3f", landingPitch * 57.2957795f, OrbitClamp(landingDuration-landingAge,0.0f,landingDuration));
            }
        }
        float t = shakeEnabled ? OrbitClamp((speed - shakeMinSpeed) / (shakeMaxSpeed - shakeMinSpeed), 0.0f, 1.0f) : 0.0f;
        t = t * t * (3.0f - 2.0f * t);
        shakeAmount += (t - shakeAmount) * (1.0f - expf(-shakeSmoothing * dt));
        shakeTime = fmodf(shakeTime + dt * shakeFrequency * 6.28318530718f, 628.318530718f);
        if (shakeAmount < 0.0001f) shakeAmount = 0.0f;
        if (shakeAmount == 0.0f && fabsf(landingPitch) < 0.000001f) return;
        shakeBaseMatrix = orbitActive ? orbitMatrix : original;
        shakeMatrix = shakeBaseMatrix;
        Vector3 position = shakeMatrix.GetRow(3);
        float amplitude = shakeIntensity * shakeAmount * 0.01745329252f;
        // Continuous blended waves, no random jumps or position changes.
        float yaw = amplitude * (0.65f * sinf(shakeTime * 0.73f) + 0.35f * sinf(shakeTime * 1.31f));
        float pitch = amplitude * (0.65f * sinf(shakeTime) + 0.35f * sinf(shakeTime * 1.57f)) + landingPitch;
        Matrix34 rot;
        rot.MakeRotate(shakeMatrix.GetRow(1), yaw);
        shakeMatrix.Dot3x3(rot);
        rot.MakeRotate(shakeMatrix.GetRow(0), pitch);
        shakeMatrix.Dot3x3(rot);
        shakeMatrix.SetRow(3, position);
        shakeActive = true;
    }

    void UpdateOrbit(const Matrix34& original) {
        orbitActive = false;
        static bool inputAttempted = false;
        if (orbitEnabled && !inputAttempted) { inputAttempted = true; LoadOrbitInput(); }
        if (!orbitEnabled || !orbitGetState || overrideCameraMtx || mirrorMode ||
            datTimeManager::SpeedrunMode || mcNetManager::IsNetworkMode) { ResetOrbit(); return; }
        mcGameState* state = mcGameState::Instance;
        if (!state || state->m_CurrentState != GameState::Game || state->m_IsPaused || state->m_IsPausedLocally) {
            ResetOrbit(); return;
        }
        mcPlayerManager* manager = mcPlayerManager::Instance;
        if (!manager || manager->m_NumPlayers < 1 || !manager->m_Players[0] || !manager->m_Players[0]->m_Entity) {
            ResetOrbit(); return;
        }
        mcCarSim* sim = manager->m_Players[0]->m_Entity->m_Car.m_CarSim;
        if (!sim || !sim->m_Collider || !sim->m_Collider->m_ICS) { ResetOrbit(); return; }
        if (sim != orbitLastCar) { ResetOrbit(); orbitLastCar = sim; }
        Vector3 pivot = sim->m_Collider->m_ICS->m_WorldTransform.GetRow(3);
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
        const Vector3& velocity = sim->m_Collider->m_ICS->m_WorldVelocity;
        float speedSq = velocity.X * velocity.X + velocity.Z * velocity.Z;
        float threshold = orbitMoving ? orbitMoveSpeed * 0.6f : orbitMoveSpeed;
        orbitMoving = speedSq >= threshold * threshold;
        if (!orbitMoving) orbitMovingTime = 0.0f;
        else orbitMovingTime = OrbitClamp(orbitMovingTime + dt, 0.0f, orbitRecenterDelay);

        if (x != 0.0f || y != 0.0f) {
            orbitYaw = remainderf(orbitYaw - x * orbitYawSpeed * rad * dt, 6.28318530718f);
            orbitPitch -= y * (orbitInvertY ? -1.0f : 1.0f) * orbitPitchSpeed * rad * dt;
        } else if (orbitMoving && orbitMovingTime >= orbitRecenterDelay) {
            float decay = expf(-orbitReturnSpeed * dt);
            orbitYaw *= decay; orbitPitch *= decay;
        }
        if (fabsf(orbitYaw) < 0.001f && fabsf(orbitPitch) < 0.001f) {
            orbitYaw = orbitPitch = 0.0f;
            orbitSafeDistance = -1.0f;
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
        orbitMatrix.SetRow(3, offset);
        orbitMatrix.RotateFullY(orbitYaw);
        // Horizontal axis perpendicular to the orbit radius, avoids camera roll.
        Vector3 radial = orbitMatrix.GetRow(3);
        Vector3 axis(radial.Z, 0.0f, -radial.X);
        if (axis.Mag2() > 0.001f) {
            axis.Normalize();
            Matrix34 rot;
            rot.MakeRotateUnitAxis(axis, orbitPitch);
            rot.SetRow(3, Vector3::ORIGIN);
            orbitMatrix.Dot(rot);
        }
        orbitMatrix.SetRow(3, orbitMatrix.GetRow(3) + pivot);
        ApplyOrbitCollision(pivot, sim, dt);
        orbitActive = true;
    }
}

// Read-only bridge for light trail motion estimation. No renderer state changes.
// The consumer verifies that the active render camera matches the shaken matrix.
bool GetLightTrailShakeMatrices(Matrix34& base, Matrix34& shaken)
{
    if (!shakeActive || overrideCameraMtx || mirrorMode) return false;
    base = shakeBaseMatrix;
    shaken = shakeMatrix;
    return true;
}

void FreeCamHandler::SetCameraHook(Matrix34& const origMtx)
{           
    if (ioKeyboard::GetKeyDown(activateFreecam) && !freecamToggle)
    {
        freecamToggle = true;
        Printf("Free camera ON\n");
        
        // Toggle override
        overrideCameraMtx = !overrideCameraMtx;

        // Executes once on buttom press, could be its own separate Init function
        if (overrideCameraMtx)
        {
            // Place the custom camera matrix at the current one
            Matrix44 currentCameraMtx44 = gfxState::GetCameraMatrix();

            // This could be done using a separate utility function converting Matrix44 to Matrix34
            // CustomCameraMtx34 = gfxRenderState::GetCameraMatrix().ToMatrix34();

            Vector4 m44a = currentCameraMtx44.GetRow(0);
            Vector4 m44b = currentCameraMtx44.GetRow(1);
            Vector4 m44c = currentCameraMtx44.GetRow(2);
            Vector4 m44d = currentCameraMtx44.GetRow(3);

            customCameraMtx34.SetRow(0, Vector3(m44a.X, m44a.Y, m44a.Z));
            customCameraMtx34.SetRow(1, Vector3(m44b.X, m44b.Y, m44b.Z));
            customCameraMtx34.SetRow(2, Vector3(m44c.X, m44c.Y, m44c.Z));
            customCameraMtx34.SetRow(3, Vector3(m44d.X, m44d.Y, m44d.Z));

            // Mirror mode adjustments
            if (mirrorMode == true)
            {
                customCameraMtx34.m00 *= -1.0f;
                customCameraMtx34.m01 *= -1.0f;
                customCameraMtx34.m02 *= -1.0f;
            }
        }
    }

    if (ioKeyboard::GetKeyUp(activateFreecam) && freecamToggle) freecamToggle = false;

    // Mouse delta values used for camera rotation
    int DX = ioMouse::GetXDelta() * mirrorFactor;
    int DY = ioMouse::GetYDelta();
    int DZ = ioMouse::GetScrollDelta();

    if (DX != 0 || DY != 0) // Update only when mouse moves
    {        
        Matrix34 ry;
        ry.MakeRotateY(-rotYSensitivity * DX * fovRotSensitivity); // Rotates left/right in world space
        customCameraMtx34.Dot3x3(ry);

        Matrix34 rx;
        rx.MakeRotate(customCameraMtx34.GetRow(0), -rotXSensitivity * DY * fovRotSensitivity); // Rotates up/down in local space
        customCameraMtx34.Dot3x3(rx);
    }

    // Camera speed adjustment with mouse scroll wheel
    if (DZ > 0 && translateMultiplier < maxTranslateMultiplier) translateMultiplier += translateMultiplierStep;
    
    if (DZ < 0 && translateMultiplier > minTranslateMultiplier) translateMultiplier -= translateMultiplierStep;

    if (DZ < 0 && translateMultiplier < minTranslateMultiplier) translateMultiplier = minTranslateMultiplier; // Overshoot safeguard

    // Move camera forth, left, back, right - relative to camera direction
    if (ioKeyboard::GetKey(freecamForward))
    {
        // Simplify these

        dir = customCameraMtx34.GetRow(2);
        e = dir * datTimeManager::GetSeconds() * translateXZSpeed * translateMultiplier;

        customCameraMtx34.m30 -= e.X;
        customCameraMtx34.m31 -= e.Y;
        customCameraMtx34.m32 -= e.Z;
    }
    if (ioKeyboard::GetKey(freecamLeft))
    {
        dir = customCameraMtx34.GetRow(0);
        e = dir * datTimeManager::GetSeconds() * translateXZSpeed * translateMultiplier * mirrorFactor;

        customCameraMtx34.m30 -= e.X;
        customCameraMtx34.m31 -= e.Y;
        customCameraMtx34.m32 -= e.Z;
    }
    if (ioKeyboard::GetKey(freecamBackward))
    {
        dir = customCameraMtx34.GetRow(2);
        e = dir * datTimeManager::GetSeconds() * translateXZSpeed * translateMultiplier;

        customCameraMtx34.m30 += e.X;
        customCameraMtx34.m31 += e.Y;
        customCameraMtx34.m32 += e.Z;
    }
    if (ioKeyboard::GetKey(freecamRight))
    {
        dir = customCameraMtx34.GetRow(0);
        e = dir * datTimeManager::GetSeconds() * translateXZSpeed * translateMultiplier * mirrorFactor;

        customCameraMtx34.m30 += e.X;
        customCameraMtx34.m31 += e.Y;
        customCameraMtx34.m32 += e.Z;
    }
    
    // Move camera down and up
    if (ioKeyboard::GetKey(freecamDown))
    {
        customCameraMtx34.m31 -= datTimeManager::GetSeconds() * translateYSpeed * translateMultiplier;
    }
    if (ioKeyboard::GetKey(freecamUp))
    {
        customCameraMtx34.m31 += datTimeManager::GetSeconds() * translateYSpeed * translateMultiplier;
    }
    
    UpdateOrbit(origMtx);
    UpdateShake(origMtx);
    if (overrideCameraMtx) gfxState::SetCamera34(customCameraMtx34);
    else if (shakeActive) gfxState::SetCamera34(shakeMatrix);
    else if (orbitActive) gfxState::SetCamera34(orbitMatrix);
    else gfxState::SetCamera34(origMtx); // Call original
}
void FreeCamHandler::SetCameraOtherHook(Matrix34& const origMtx) // Only set the matrix, no other calculations
{
    if (overrideCameraMtx) gfxState::SetCamera34(customCameraMtx34);
    else if (shakeActive) gfxState::SetCamera34(shakeMatrix);
    else if (orbitActive) gfxState::SetCamera34(orbitMatrix);
    else gfxState::SetCamera34(origMtx); // Call original
}
void FreeCamHandler::SetWorldHook(Matrix34& const origMtx)
{
    if (overrideCameraMtx)
    {
        customSkyboxCameraMtx34 = origMtx;
        customSkyboxCameraMtx34.SetRow(3, customCameraMtx34.GetRow(3));
        gfxState::SetWorld34(customSkyboxCameraMtx34); // Call using custom matrix
    }
    else if (orbitActive)
    {
        // Center skybox and starfield on the effective orbit camera.
        // Keep native world orientation; the view already contains orbit rotation.
        customSkyboxCameraMtx34 = origMtx;
        customSkyboxCameraMtx34.SetRow(3, (shakeActive ? shakeMatrix : orbitMatrix).GetRow(3));
        gfxState::SetWorld34(customSkyboxCameraMtx34);
    }
    else gfxState::SetWorld34(origMtx); // Call original
}
void FreeCamHandler::AddFrameCameraHook(Matrix34& const a2, bool& const a3)
{
    if (overrideCameraMtx) audioMtx = customCameraMtx34;
    else audioMtx = a2;

    if (mirrorMode)
    {
        audioMtx.m00 *= -1.0f;
        audioMtx.m01 *= -1.0f;
        audioMtx.m02 *= -1.0f;
    }

    hook::Thunk<0x5D6050>::Call<void>(this, &audioMtx, false); // Call using custom audio matrix
}
void FreeCamHandler::sub_518FA0_Hook(Vector3& const a2, void* a3, void* a4)
{
    if (overrideCameraMtx)
    {
        a2.X = customCameraMtx34.m30;
        a2.Z = customCameraMtx34.m32;
        hook::Thunk<0x518FA0>::Call<void>(this, &a2, a3, a4); // Call using custom camera position
    }
    else
    {
        hook::Thunk<0x518FA0>::Call<void>(this, &a2, a3, a4); // Call original
    }
}
void FreeCamHandler::PerspectiveHook(float fov, float& const aspect, float& const nearClip, float& const farClip)
{    
    if (overrideCameraMtx)
    {
        // Custom FOV logic
        if (ioKeyboard::GetKey(freecamIncreaseFOV))
        {
            if (customFOV < maxFOV)
            {
                customFOV += fovStep * datTimeManager::GetSeconds();// Increase FOV (FPS independent)
                fovRotSensitivity = GetFOVLerpRate(0.25f, 2.0f, customFOV/maxFOV);
            }
        }

        if (ioKeyboard::GetKey(freecamDecreaseFOV))
        {
            if (ceil(customFOV) > minFOV)
            {
                customFOV -= fovStep * datTimeManager::GetSeconds();// Decrease FOV (FPS independent)
                fovRotSensitivity = GetFOVLerpRate(0.25f, 2.0f, customFOV / maxFOV);
            }
        }

        //gfxViewport::Perspective(10.0f, &aspect, &nearClip, &farClip);
        hook::Thunk<0x5FA880>::Call<void>(this, customFOV, &aspect, cameraNearClip, cameraFarClip);
    }
    else
    {
        //gfxViewport::Perspective(&fov, &aspect, &nearClip, &farClip); // Call original
        hook::Thunk<0x5FA880>::Call<void>(this, fov, &aspect, cameraNearClip, cameraFarClip); // Call original
        customFOV = fov; // Grab current FOV
    }
}
void FreeCamHandler::ToggleCinematicCamHook()
{
    hook::Thunk<0x4045E0>::Call<void>(this); // Call original function (some mcGameState Update it seems)

    if (ioKeyboard::GetKeyDown(activateReplayCam)) //(GetAsyncKeyState(0x69) & 1) // NUMPAD9
    {
        Printf("Cinematic camera toggled\n");
        currentGameState = *getPtr<int>(this, 0x4);
        
        if (currentGameState == 2) // If current state is GAMEPLAY
        {
            *getPtr<int>(this, 0x4) = 3; // Switch to REPLAY state
            // Look into the original EnterStateReplay function and see how it handles car LODs
        }
        else if (currentGameState == 3) // If current state is REPLAY
        {
            *getPtr<int>(this, 0x4) = 2; // Switch to GAMEPLAY state
            // Need to refresh camera here, do it manually in game for now
        }
    }
}
float FreeCamHandler::GetFOVLerpRate(float a, float b, float t) // Regular lerp function, replace it later
{
    return a + (b - a) * t;
}

void FreeCamHandler::Install()
{    
    bool enableFreecam = HookConfig::GetBool("Freecam", "EnableFreecam", true);

    if (enableFreecam)
    {
        orbitEnabled = HookConfig::GetBool("OrbitCamera", "EnableOrbitCamera", false);
        orbitYawSpeed = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "HorizontalSpeed", 180.0f), 10.0f, 720.0f);
        orbitPitchSpeed = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "VerticalSpeed", 90.0f), 0.0f, 360.0f);
        orbitDeadZone = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "DeadZone", 0.18f), 0.0f, 0.95f);
        orbitReturnSpeed = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "ReturnSpeed", 5.0f), 0.1f, 20.0f);
        orbitInvertY = HookConfig::GetBool("OrbitCamera", "InvertY", false);
        orbitRecenterDelay = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "RecenterDelay", 1.5f), 0.0f, 10.0f);
        orbitMoveSpeed = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "MoveSpeedThreshold", 0.5f), 0.1f, 10.0f);
        orbitCollision = HookConfig::GetBool("OrbitCamera", "EnableCollision", true);
        orbitCollisionRadius = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "CollisionRadius", 0.35f), 0.1f, 1.0f);
        orbitCollisionPadding = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "CollisionPadding", 0.10f), 0.0f, 0.5f);
        orbitCollisionRecovery = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "CollisionRecovery", 8.0f), 0.5f, 30.0f);
        orbitMinElevation = OrbitClamp(HookConfig::GetFloat("OrbitCamera", "MinElevation", -10.0f), -75.0f, 30.0f);

        landingEnabled = HookConfig::GetBool("LandingShake", "EnableLandingShake", false);
        landingDebug = HookConfig::GetBool("LandingShake", "Debug", false);

        landingIntensity = OrbitClamp(HookConfig::GetFloat("LandingShake", "Intensity", 0.8f), 0.0f, 5.0f);
        landingDuration = OrbitClamp(HookConfig::GetFloat("LandingShake", "Duration", 0.24f), 0.10f, 0.80f);
        hook_output("[LandingShake] v0.5.2b enabled=%d debug=%d intensity=%.2f duration=%.2f", int(landingEnabled), int(landingDebug), landingIntensity, landingDuration);
        shakeEnabled = HookConfig::GetBool("CameraShake", "EnableCameraShake", false);
        shakeMinSpeed = OrbitClamp(HookConfig::GetFloat("CameraShake", "MinSpeed", 100.0f), 0.0f, 999.0f);
        shakeMaxSpeed = OrbitClamp(HookConfig::GetFloat("CameraShake", "MaxSpeed", 280.0f), shakeMinSpeed + 1.0f, 1000.0f);
        shakeIntensity = OrbitClamp(HookConfig::GetFloat("CameraShake", "Intensity", 0.15f), 0.0f, 3.0f);
        shakeFrequency = OrbitClamp(HookConfig::GetFloat("CameraShake", "Frequency", 12.0f), 0.1f, 20.0f);
        shakeSmoothing = OrbitClamp(HookConfig::GetFloat("CameraShake", "Smoothing", 5.0f), 0.1f, 30.0f);

        // https://community.bistudio.com/wiki/DIK_KeyCodes
        // Read key mappings from .ini file
        activateFreecam = HookConfig::GetInt("Input Setup", "ActivateFreecam", DIK_F4);
        freecamLeft = HookConfig::GetInt("Input Setup", "FreecamLeft", DIK_J);
        freecamRight = HookConfig::GetInt("Input Setup", "FreecamRight", DIK_L);
        freecamForward = HookConfig::GetInt("Input Setup", "FreecamForward", DIK_I);
        freecamBackward = HookConfig::GetInt("Input Setup", "FreecamBackward", DIK_K);
        freecamUp = HookConfig::GetInt("Input Setup", "FreecamUp", DIK_O);
        freecamDown = HookConfig::GetInt("Input Setup", "FreecamDown", DIK_U);
        freecamIncreaseFOV = HookConfig::GetInt("Input Setup", "FreecamIncreaseFOV", DIK_P);
        freecamDecreaseFOV = HookConfig::GetInt("Input Setup", "FreecamDecreaseFOV", DIK_SEMICOLON);
        activateReplayCam = HookConfig::GetInt("Input Setup", "ActivateReplayCam", DIK_F3);

        // Mirror mode check
        mirrorMode = HookConfig::GetBool("Graphics", "MirrorMode", false);
        if (mirrorMode) mirrorFactor = -1.0f;

        // Read camera clip values
        cameraNearClip = HookConfig::GetFloat("Graphics", "CameraNearClip", 0.25f);
        cameraFarClip = HookConfig::GetFloat("Graphics", "CameraFarClip", 2000.0f);

        // Read freecam variables
        translateXZSpeed = HookConfig::GetFloat("Freecam", "TranslateXZSpeed", 20.0f); // Speed on the XZ plane
        translateYSpeed = HookConfig::GetFloat("Freecam", "TranslateYSpeed", 8.0f); // Up/down speed
        translateMultiplierStep = HookConfig::GetFloat("Freecam", "TranslateMultiplierStep", 0.075f);
        minTranslateMultiplier = HookConfig::GetFloat("Freecam", "MinTranslateMultiplier", 0.06f);
        maxTranslateMultiplier = HookConfig::GetFloat("Freecam", "MaxTranslateMultiplier", 5.0f);
        rotYSensitivity = HookConfig::GetFloat("Freecam", "RotYSensitivity", 0.0009f);
        rotXSensitivity = HookConfig::GetFloat("Freecam", "RotXSensitivity", 0.0005f);
        minFOV = HookConfig::GetFloat("Freecam", "MinFOV", 5.0f);
        maxFOV = HookConfig::GetFloat("Freecam", "MaxFOV", 120.0f);
        fovStep = HookConfig::GetFloat("Freecam", "FOVStep", 20.0f);

        InstallCallback("Free Cam Handler (1)", "Free camera",
            &SetCameraHook, {
                //cb::call(0x4A6D7E) // Minimap related
                cb::call(0x517C3F), // Cam related
            });

        InstallCallback("Free Cam Handler (2)", "Free camera other",
            &SetCameraOtherHook, {
                cb::call(0x57C36A) // Headlight related
            });

        InstallCallback("Free Cam Handler (4)", "Free camera 3D sound",
            &AddFrameCameraHook, {
                cb::call(0x469C79), // 3D sound related
            });

        InstallCallback("Free Cam Handler (5)", "Free camera PVS position",
            &sub_518FA0_Hook, {
                cb::call(0x524077), // PVS camera related
            });

        InstallCallback("Free Cam Handler (6)", "Free camera SetWorld",
            &SetWorldHook, {
                cb::call(0x520662), // Skybox related
                cb::call(0x5206C3), // Starfield related
            });

        InstallCallback("Free Cam Handler (7)", "Free camera FOV",
            &PerspectiveHook, {
                cb::call(0x57C398), // FOV related
            });

        InstallVTableHook("Cinematic Cam Handler", &ToggleCinematicCamHook, { 0x62D970 });
    }
}
