#pragma once
#include <mc2hook/mc2hook.h>
#include <framework/HookConfig.h>
#include <age/data/timemgr.h>
#include <age/state/gamestate.h>
#include <age/mcgame/playermgr.h>
#include <age/mcgame/player.h>
#include <age/vehicle/entity.h>
#include <mccar/carsim.h>
#include <age/physics/phcollider.h>
#include <age/physics/phinertia.h>
#include <age/gfx/rstate.h>
#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <cstdint>

class MotionBlurHandler {
    struct TLVertex {
        float x, y, z, rhw;
        D3DCOLOR color;
        float u, v;
    };

    enum { FVF = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1 };
    using Present_t = HRESULT (WINAPI*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
    using Reset_t   = HRESULT (WINAPI*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

    static inline Present_t s_OriginalPresent = nullptr;
    static inline Reset_t s_OriginalReset = nullptr;
    static inline bool s_Hooked = false;
    static inline bool s_Busy = false;
    static inline bool s_PreHudDrawn = false;
    static inline bool s_PreHudTried = false;
    static inline bool s_PreHudInstalled = false;
    // StretchRect is not generally legal inside BeginScene/EndScene.
    // Resume the game's scene on every return after a successful split.
    struct SceneResume {
        IDirect3DDevice9* device;
        bool pending = false;
        ~SceneResume() {
            if (pending) {
                const HRESULT hr = device->BeginScene();
                if (FAILED(hr)) MotionBlurHandler::Report("PreHUD resume BeginScene", hr);
            }
        }
    };
    static inline HANDLE s_Thread = nullptr;
    static void Report(const char* stage, HRESULT hr) {
        static ULONGLONG last=0; const auto now=GetTickCount64();
        if (!last || now-last>=2000) { last=now; hook_output("[MotionBlur] %s: 0x%08X",stage,(unsigned)hr); }
    }

    static inline IDirect3DTexture9* s_SourceTexture = nullptr;
    static inline IDirect3DSurface9* s_SourceSurface = nullptr;
    static inline UINT s_RTWidth = 0, s_RTHeight = 0;
    static inline D3DFORMAT s_RTFormat = D3DFMT_UNKNOWN;
    static inline float s_BlurAmount = 0.0f;
    static inline ULONGLONG s_LastTicks = 0;

    static float Clamp(float x, float lo, float hi) {
        return x < lo ? lo : (x > hi ? hi : x);
    }
    static float Saturate(float x) { return Clamp(x, 0.0f, 1.0f); }

    static bool Enabled() {
        static bool enabled = HookConfig::GetBool("MotionBlur", "EnableMotionBlur", false);
        return enabled;
    }
    static float ConfigF(const char* key, float def, float lo, float hi) {
        const float v = HookConfig::GetFloat("MotionBlur", key, def);
        return std::isfinite(v) ? Clamp(v, lo, hi) : def;
    }
    static int ConfigI(const char* key, int def, int lo, int hi) {
        const int v = HookConfig::GetInt("MotionBlur", key, def);
        return v < lo ? lo : (v > hi ? hi : v);
    }

    static float GetPlayerSpeedKmh() {
        mcGameState* state = mcGameState::Instance;
        if (!state || state->m_CurrentState != GameState::Game || state->m_IsPaused || state->m_IsPausedLocally) {
            return 0.0f;
        }
        mcPlayerManager* manager = mcPlayerManager::Instance;
        if (!manager || manager->m_NumPlayers < 1 || !manager->m_Players[0] || !manager->m_Players[0]->m_Entity) {
            return 0.0f;
        }
        mcCarSim* sim = manager->m_Players[0]->m_Entity->m_Car.m_CarSim;
        if (!sim || !sim->m_Collider || !sim->m_Collider->m_ICS) {
            return 0.0f;
        }
        const Vector3& vel = sim->m_Collider->m_ICS->m_WorldVelocity;
        const float speedMs = std::sqrt(vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z);
        return speedMs * 3.6f;
    }

    static void ReleaseResources() {
        if (s_SourceSurface) { s_SourceSurface->Release(); s_SourceSurface = nullptr; }
        if (s_SourceTexture) { s_SourceTexture->Release(); s_SourceTexture = nullptr; }
        s_RTWidth = s_RTHeight = 0;
        s_RTFormat = D3DFMT_UNKNOWN;
    }

    static void EnsureResources(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format) {
        if (s_SourceTexture && s_RTWidth == width && s_RTHeight == height && s_RTFormat == format) return;
        ReleaseResources();
        if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                                         format, D3DPOOL_DEFAULT,
                                         &s_SourceTexture, nullptr))) {
            return;
        }
        if (FAILED(s_SourceTexture->GetSurfaceLevel(0, &s_SourceSurface))) {
            ReleaseResources();
            return;
        }
        s_RTWidth = width;
        s_RTHeight = height;
        s_RTFormat = format;
    }

    static D3DCOLOR VertexColor(float alpha) {
        const DWORD a = DWORD(Saturate(alpha) * 255.0f + 0.5f);
        return D3DCOLOR_ARGB(a, 255, 255, 255);
    }

    static float SmoothStep(float x) {
        x = Saturate(x);
        return x*x*(3.0f-2.0f*x);
    }
    // Rounded U in screen-height units: one distance field and one feather.
    static float MaskAlphaUShaped(float fx, float fy, float aspect) {
        constexpr float cx = 0.50f;
        static const float width = ConfigF("MaskWidth", 0.24f, 0.08f, 0.40f);
        static const float bottom = ConfigF("MaskBottomBias", 0.48f, 0.35f, 0.92f);
        static const float height = ConfigF("MaskHeight", 0.20f, 0.06f, 0.34f);
        static const float feather = ConfigF("MaskFalloff", 0.297f, 0.02f, 1.50f);
        // Open-ended rounded rectangle: straight arms, circular lower corners.
        const float halfW = width * aspect;
        const float radius = height < halfW ? height : halfW;
        const float qx = std::fabs(fx-cx)*aspect - (halfW-radius);
        const float qy = fy-bottom;
        const float ox = qx>0.0f?qx:0.0f;
        const float oy = qy>0.0f?qy:0.0f;
        const float largest = qx>qy?qx:qy;
        const float distance = std::sqrt(ox*ox+oy*oy) + (largest<0.0f?largest:0.0f) - radius;
        // Same physical feather on the sides, corners and bottom.
        return SmoothStep(distance/feather);
    }
    // Elliptical HUD protection, with feathering outside the protected core.
    static float ProtectEllipse(float x,float y,float cx,float cy,float rx,float ry,float feather) {
        const float dx=(x-cx)/rx, dy=(y-cy)/ry;
        return SmoothStep((std::sqrt(dx*dx+dy*dy)-1.0f)/feather);
    }
    static float HUDMask(float x,float y) {
        // Normalized coordinates calibrated against the supplied 16:9 screenshots.
        float a=ProtectEllipse(x,y,0.069f,0.865f,0.051f,0.090f,0.55f);
        a*=ProtectEllipse(x,y,0.923f,0.865f,0.057f,0.096f,0.55f);
        // Narrow vertical nitro gauge, not the entire right-hand rectangle.
        a*=ProtectEllipse(x,y,0.955f,0.687f,0.019f,0.163f,0.60f);
        a*=ProtectEllipse(x,y,0.981f,0.020f,0.017f,0.025f,0.65f);
        return a;
    }

    static void BuildGrid(std::vector<TLVertex>& vertices, std::vector<WORD>& indices,
                          UINT width, UINT height,
                          float uvScale, float passAlpha, bool protectHUD) {
        const int cols = 65;
        const int rows = 49;
        vertices.clear();
        indices.clear();
        vertices.reserve(cols * rows);
        indices.reserve((cols - 1) * (rows - 1) * 6);

        for (int y = 0; y < rows; ++y) {
            const float fy = float(y) / float(rows - 1);
            for (int x = 0; x < cols; ++x) {
                const float fx = float(x) / float(cols - 1);
                float mask = MaskAlphaUShaped(fx, fy, float(width)/float(height));
                if (protectHUD) mask *= HUDMask(fx,fy);

                TLVertex v{};
                v.x = fx * float(width) - 0.5f;
                v.y = fy * float(height) - 0.5f;
                v.z = 0.0f;
                v.rhw = 1.0f;
                v.color = VertexColor(mask * passAlpha);
                v.u = 0.5f + (fx - 0.5f) * uvScale;
                v.v = 0.5f + (fy - 0.5f) * uvScale;
                if (protectHUD) v.color = VertexColor(mask * HUDMask(v.u,v.v) * passAlpha);
                vertices.push_back(v);
            }
        }

        for (int y = 0; y < rows - 1; ++y) {
            for (int x = 0; x < cols - 1; ++x) {
                const WORD i0 = WORD(y * cols + x);
                const WORD i1 = WORD(y * cols + x + 1);
                const WORD i2 = WORD((y + 1) * cols + x);
                const WORD i3 = WORD((y + 1) * cols + x + 1);
                indices.push_back(i0); indices.push_back(i1); indices.push_back(i2);
                indices.push_back(i2); indices.push_back(i1); indices.push_back(i3);
            }
        }
    }

    static void ApplyBlur(IDirect3DDevice9* device, bool preHud = false) {
        if (s_Busy || !Enabled()) return;
        if (datTimeManager::SpeedrunMode || device != gfxState::lpD3DDevice.get()) return;
        auto* game = mcGameState::Instance.get();
        if (!game || game->m_CurrentState != GameState::Game || game->m_IsPaused || game->m_IsPausedLocally) { s_BlurAmount=0; return; }

        static const int debugMode = ConfigI("DebugMode", 0, 0, 3);
        static const float minSpeed = ConfigF("MinSpeed", 75.0f, 0.0f, 400.0f);
        static const float maxSpeed = ConfigF("MaxSpeed", 230.0f, minSpeed + 1.0f, 600.0f);
        static const float maxIntensity = ConfigF("Intensity", 0.050f, 0.0f, 0.20f);
        static const int samples = ConfigI("Samples", 16, 2, 32);
        static const float speedCurve = ConfigF("SpeedCurve", 1.20f, 0.25f, 4.0f);
        static const float smoothing = ConfigF("Smoothing", 5.0f, 0.1f, 20.0f);

        const ULONGLONG ticks = GetTickCount64();
        float dt = 1.0f / 60.0f;
        if (s_LastTicks && ticks > s_LastTicks) dt = Clamp(float(ticks - s_LastTicks) * 0.001f, 0.0f, 0.05f);
        s_LastTicks = ticks;

        const float speed = GetPlayerSpeedKmh();
        constexpr bool debug=false;
        static ULONGLONG lastLog=0;
        if(debug && ticks-lastLog>=2000) { lastLog=ticks; hook_output("[MotionBlur] Speed %.1f km/h, blur %.4f",speed,s_BlurAmount); }
        const float target = maxIntensity * std::pow(Saturate((speed - minSpeed) / (maxSpeed - minSpeed)), speedCurve);
        const float k = 1.0f - std::exp(-smoothing * dt);
        s_BlurAmount += (target - s_BlurAmount) * k;
        if (debugMode == 0 && s_BlurAmount < 0.0005f) return;

        // In the HUD pass, use its current destination rather than guessing
        // that it is the swap chain. Only full-size targets are eligible.
        IDirect3DSurface9* backBuffer = nullptr;
        HRESULT bbHr = preHud ? device->GetRenderTarget(0,&backBuffer)
                              : device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&backBuffer);
        if (FAILED(bbHr) || !backBuffer) { Report("GetBackBuffer",bbHr); return; }
        D3DSURFACE_DESC desc{};
        if (FAILED(backBuffer->GetDesc(&desc))) { backBuffer->Release(); return; }
        if (preHud) {
            IDirect3DSurface9* bb = nullptr;
            D3DSURFACE_DESC bd = {};
            HRESULT r = device->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb);
            bool full = SUCCEEDED(r) && bb && SUCCEEDED(bb->GetDesc(&bd)) &&
                        bd.Width == desc.Width && bd.Height == desc.Height;
            if (bb) bb->Release();
            if (!full) { Report("PreHUD non-full-size target", E_FAIL); backBuffer->Release(); return; }
        }
        if (debug || debugMode) {
            static ULONGLONG lastRT=0;
            if (!lastRT || ticks-lastRT>=2000) {
                lastRT=ticks;
                hook_output("[MotionBlur] %s RT=%ux%u fmt=%u MSAA=%u",preHud?"PreHUD":"Present",desc.Width,desc.Height,(unsigned)desc.Format,(unsigned)desc.MultiSampleType);
            }
        }
        if (debugMode != 1 && desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8) {
            Report("Unsupported render target format",(HRESULT)desc.Format);
            backBuffer->Release();
            return;
        }
        if(debugMode != 1) EnsureResources(device, desc.Width, desc.Height, desc.Format);
        if (debugMode != 1 && !s_SourceSurface) { Report("Source allocation failed",E_FAIL); backBuffer->Release(); return; }

        HRESULT hr=S_OK;
        SceneResume resume{device};
        IDirect3DStateBlock9* stateBlock = nullptr;
        hr=device->CreateStateBlock(D3DSBT_ALL, &stateBlock);
        if (FAILED(hr) || !stateBlock) { Report("CreateStateBlock",hr); backBuffer->Release(); return; }
        hr=stateBlock->Capture();
        if (FAILED(hr)) { Report("Capture",hr); stateBlock->Release(); backBuffer->Release(); return; }
        std::vector<TLVertex> verts; std::vector<WORD> inds;
        verts.reserve(65*49); inds.reserve(64*48*6);
        // State blocks do not save render targets or depth-stencil surfaces.
        IDirect3DSurface9* oldRT=nullptr;
        IDirect3DSurface9* oldDepth=nullptr;
        D3DVIEWPORT9 oldViewport={};
        RECT oldScissor={};
        hr=device->GetRenderTarget(0,&oldRT);
        if (FAILED(hr) || !oldRT) { Report("GetRenderTarget",hr); stateBlock->Release(); backBuffer->Release(); return; }
        const HRESULT depthHr=device->GetDepthStencilSurface(&oldDepth);
        if ((FAILED(depthHr) && depthHr!=D3DERR_NOTFOUND) || FAILED(device->GetViewport(&oldViewport)) || FAILED(device->GetScissorRect(&oldScissor))) {
            Report("Save attachments/viewport",E_FAIL);
            if(oldDepth) oldDepth->Release(); oldRT->Release(); stateBlock->Release(); backBuffer->Release(); return;
        }
        // Do not disturb active MRT configurations; MC2 normally uses just RT0.
        D3DCAPS9 caps={};
        bool safe=SUCCEEDED(device->GetDeviceCaps(&caps));
        for(UINT i=1; safe && i<caps.NumSimultaneousRTs; ++i) {
            IDirect3DSurface9* extra=nullptr;
            HRESULT r=device->GetRenderTarget(i,&extra);
            if(extra) { extra->Release(); safe=false; }
            else if(FAILED(r) && r!=D3DERR_NOTFOUND) safe=false;
        }
        if(!safe) { Report("MRT configuration skipped",E_FAIL); if(oldDepth) oldDepth->Release(); oldRT->Release(); stateBlock->Release(); backBuffer->Release(); return; }
        // Save attachments and states before splitting the active game scene.
        // The red diagnostic needs no texture copy and can stay inside it.
        if (preHud && debugMode != 1) {
            hr=device->EndScene();
            if (FAILED(hr)) {
                Report("PreHUD EndScene",hr);
                if(oldDepth) oldDepth->Release(); oldRT->Release(); stateBlock->Release(); backBuffer->Release(); return;
            }
            resume.pending=true;
        }
        if (debugMode != 1) {
            hr=device->StretchRect(backBuffer,nullptr,s_SourceSurface,nullptr,D3DTEXF_NONE);
            if(FAILED(hr)) {
                Report(preHud?"PreHUD StretchRect":"Present StretchRect",hr);
                if(oldDepth) oldDepth->Release(); oldRT->Release(); stateBlock->Release(); backBuffer->Release(); return;
            }
        }
        s_Busy=true;
        bool ready=true;
#define MB_SET(call) do { if(ready) { HRESULT r=device->call; if(FAILED(r)) { Report(#call,r); ready=false; } } } while(0)
        MB_SET(SetDepthStencilSurface(nullptr));
        MB_SET(SetRenderTarget(0,backBuffer));
        bool began=false;
        if(ready && !(preHud && debugMode==1)) { hr=device->BeginScene(); began=SUCCEEDED(hr); if(!began) { Report("Blur BeginScene",hr); ready=false; } }
        MB_SET(SetVertexShader(nullptr));
        MB_SET(SetPixelShader(nullptr));
        MB_SET(SetRenderState(D3DRS_STENCILENABLE,FALSE));
        MB_SET(SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE));
        MB_SET(SetRenderState(D3DRS_COLORWRITEENABLE,15));
        MB_SET(SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
        MB_SET(SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
        MB_SET(SetRenderState(D3DRS_MULTISAMPLEMASK,0xffffffffu));
        MB_SET(SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE));
        D3DVIEWPORT9 viewport = {0,0,desc.Width,desc.Height,0.0f,1.0f};
        MB_SET(SetViewport(&viewport));
        MB_SET(SetRenderState(D3DRS_CLIPPLANEENABLE,0));
        MB_SET(SetTexture(0, debugMode == 1 ? nullptr : s_SourceTexture));
        MB_SET(SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0));
        MB_SET(SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
        MB_SET(SetFVF(FVF));
        MB_SET(SetRenderState(D3DRS_ZENABLE, FALSE));
        MB_SET(SetRenderState(D3DRS_ZWRITEENABLE, FALSE));
        MB_SET(SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE));
        MB_SET(SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA));
        MB_SET(SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA));
        MB_SET(SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE));
        MB_SET(SetRenderState(D3DRS_FOGENABLE, FALSE));
        MB_SET(SetRenderState(D3DRS_LIGHTING, FALSE));
        MB_SET(SetRenderState(D3DRS_ALPHATESTENABLE, FALSE));
        MB_SET(SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE));
        MB_SET(SetTextureStageState(0, D3DTSS_COLOROP, debugMode == 1 ? D3DTOP_SELECTARG2 : D3DTOP_MODULATE));
        MB_SET(SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE));
        MB_SET(SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE));
        MB_SET(SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG2));
        MB_SET(SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE));
        MB_SET(SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE));
        MB_SET(SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE));
        MB_SET(SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE));
        MB_SET(SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE));
        MB_SET(SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP));
        MB_SET(SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP));
        MB_SET(SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR));
        MB_SET(SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR));
        MB_SET(SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE));

        const int passes = (debugMode == 1 || debugMode == 2) ? 1 : samples;
        unsigned submitted=0;
        for (int i = 0; ready && i < passes; ++i) {
            const float t = (float(i) + 0.5f) / float(samples);
            const float scale = debugMode == 2 ? 1.0f : 1.0f - (debugMode == 3 ? 0.35f : s_BlurAmount) * t;
            // Equal sample weights at full mask; distribute partial coverage across taps.
            // Increasing sample count does not increase total edge opacity without bound.
            const float passAlpha = 1.0f / float(i+2);
            BuildGrid(verts, inds, desc.Width, desc.Height, debugMode==1 ? 1.0f : scale, debugMode==1 ? 1.0f : passAlpha, !preHud && (debugMode==0 || debugMode==1));
            if(debugMode == 1) { for(auto& v : verts) { const DWORD a=(v.color>>24)&255u; v.color=D3DCOLOR_ARGB(a,255,0,0); } }
            if(debugMode == 2) { for(auto& v : verts) { v.u-=0.12f; v.color=0xffffffffu; } }
            if (!verts.empty() && !inds.empty()) {
                hr=device->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, (UINT)verts.size(),
                                               (UINT)inds.size() / 3, inds.data(), D3DFMT_INDEX16,
                                               verts.data(), sizeof(TLVertex));
                if(FAILED(hr)) { Report("DrawIndexedPrimitiveUP",hr); ready=false; } else ++submitted;
            }
        }

        if(debugMode || debug) { static ULONGLONG lastDraw=0; if(!lastDraw || ticks-lastDraw>=2000) { lastDraw=ticks; hook_output("[MotionBlur] %s mode=%d submitted=%u/%d RT=%ux%u",preHud?"PreHUD":"Present",debugMode,submitted,passes,desc.Width,desc.Height); } }
        #undef MB_SET
        if(began) { hr=device->EndScene(); if(FAILED(hr)) Report("Present EndScene",hr); }
        // Restore attachments first: binding RT0 resets the viewport.
        hr=device->SetRenderTarget(0,oldRT); if(FAILED(hr)) Report("Restore RT0",hr);
        hr=device->SetDepthStencilSurface(oldDepth); if(FAILED(hr)) Report("Restore depth",hr);
        hr=stateBlock->Apply(); if(FAILED(hr)) Report("Restore state",hr);
        hr=device->SetViewport(&oldViewport); if(FAILED(hr)) Report("Restore viewport",hr);
        hr=device->SetScissorRect(&oldScissor); if(FAILED(hr)) Report("Restore scissor",hr);
        stateBlock->Release();
        if(oldDepth) oldDepth->Release();
        oldRT->Release(); backBuffer->Release();
        s_Busy=false;
        if (preHud && submitted > 0) s_PreHudDrawn=true;
    }

    // 0x4996C3 calls the native 2D state setup before the HUD component list.
    // Verified in the supplied executable; ordering/visibility still needs a game test.
    static void __cdecl Hook_HudSetup() {
        if (s_Hooked && !s_PreHudTried && !s_Busy) {
            s_PreHudTried=true;
            static ULONGLONG last=0;
            const auto now=GetTickCount64();
            static const int debugMode = ConfigI("DebugMode", 0, 0, 3);
            if (debugMode != 0 && (!last || now-last>=2000)) { last=now; hook_output("[MotionBlur] PreHUD common setup reached (0x4996C3)."); }
            auto* device=gfxState::lpD3DDevice.get();
            if (device) ApplyBlur(device,true);
        }
        // Let the engine establish its own HUD render state after our pass.
        reinterpret_cast<void (__cdecl*)()>(0x5EE9F0)();
    }

    static void InstallPreHud() {
        if (reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))!=0x400000) return;
        const unsigned char expected[] = {0xE8,0x28,0x53,0x15,0x00};
        if (std::memcmp(reinterpret_cast<const void*>(0x4996C3),expected,sizeof(expected))!=0) {
            hook_output("[MotionBlur] PreHUD unsupported call site; Present fallback."); return;
        }
        InstallCallback("MotionBlurHandler::Hook_HudSetup", "Before common HUD component rendering", &Hook_HudSetup, {cb::call(0x4996C3)});
        s_PreHudInstalled=true;
        hook_output("[MotionBlur] PreHUD common setup hook installed (0x4996C3).");
    }

    static HRESULT WINAPI Hook_Present(IDirect3DDevice9* device,const RECT* src,const RECT* dst,HWND hwnd,const RGNDATA* dirty) {
        // Avoid fallback cutouts during cinematics/countdown unless explicitly requested.
        if (!s_PreHudDrawn && !s_PreHudInstalled && !s_PreHudTried) ApplyBlur(device);
        s_PreHudDrawn=false;
        s_PreHudTried=false;
        return s_OriginalPresent(device,src,dst,hwnd,dirty);
    }

    static HRESULT WINAPI Hook_Reset(IDirect3DDevice9* device, D3DPRESENT_PARAMETERS* params) {
        ReleaseResources();
        s_PreHudDrawn=s_PreHudTried=false;
        return s_OriginalReset ? s_OriginalReset(device, params) : D3D_OK;
    }

    static DWORD WINAPI HookThread(LPVOID) {
        for (;;) {
            if (s_Hooked || !Enabled()) return 0;
            IDirect3DDevice9* device = gfxState::lpD3DDevice;
            if (device) {
                void** vtbl = *reinterpret_cast<void***>(device);
                if (vtbl) {
                    DWORD oldProtect = 0;
                    if (VirtualProtect(&vtbl[16], sizeof(void*) * 2, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                        s_OriginalReset = reinterpret_cast<Reset_t>(vtbl[16]);
                        s_OriginalPresent = reinterpret_cast<Present_t>(vtbl[17]);
                        vtbl[16] = reinterpret_cast<void*>(&Hook_Reset);
                        vtbl[17] = reinterpret_cast<void*>(&Hook_Present);
                        VirtualProtect(&vtbl[16], sizeof(void*) * 2, oldProtect, &oldProtect);
                        s_Hooked = true;
                        hook_output("[MotionBlur] D3D9 Present/Reset hooks installed (v0.2.4).");
                        return 0;
                    }
                }
            }
            Sleep(250);
        }
    }

public:
    static void Install() {
        if (!Enabled()) return;
        if (s_Thread) return;
        InstallPreHud();
        s_Thread = CreateThread(nullptr, 0, &HookThread, nullptr, 0, nullptr);
        if (s_Thread) {
            CloseHandle(s_Thread);
            hook_output("[MotionBlur] Waiting for D3D9 device...");
        }
    }
};
