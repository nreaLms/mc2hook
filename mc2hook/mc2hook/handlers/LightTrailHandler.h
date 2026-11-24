#pragma once
#include <mc2hook/mc2hook.h>
#include <age/age.h>
#include <age/state/gamestate.h>
#include <age/types.h>
#include <age/data/timemgr.h>
#include <age/gfx/rstate.h>
#include <d3d9.h>
#include <age/physics/phlevel.h>
#include <age/physics/phcollider.h>
#include <age/mcgame/playermgr.h>
#include <age/mcgame/player.h>
#include <age/vehicle/entity.h>
#include <mccar/carsim.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <array>
#include <map>

// Implemented by the Orbit Camera v0.4 compatibility bridge.
bool GetLightTrailShakeMatrices(Matrix34& base, Matrix34& shaken);

// Continuous, screen-space additive streaks for the native glowstar batch.
// Addresses validated against the supplied MC2 executable; no sprite duplication.
class LightTrailHandler {
    struct Sprite { float x, y, z, size; unsigned int color; };
    struct Meta { unsigned char a, b; };
    struct Mat { float m[4][4]; };
    struct Vec { float v[4]; };
    struct Vertex { float x,y,z,rhw; DWORD color; };
    struct History { Mat previous; LONGLONG time; unsigned int frame; DWORD width,height,x,y; bool valid; };
    static_assert(sizeof(Sprite)==20 && sizeof(Meta)==2 && sizeof(Vertex)==20, "Unexpected native layout");
    using DrawFn=void (__cdecl*)(int,const Sprite*,const Meta*,int);
    static History& State() { static History h={}; return h; }
    static float Clamp(float x,float lo,float hi) { return x<lo?lo:(x>hi?hi:x); }
    static float Setting(const char* key,float fallback,float lo,float hi) {
        const float x=HookConfig::GetFloat("LightTrails",key,fallback);
        return std::isfinite(x)?Clamp(x,lo,hi):fallback;
    }
    static Vec Mul(const Vec& p,const Mat& m) {
        Vec r={}; for(int j=0;j<4;++j) for(int i=0;i<4;++i) r.v[j]+=p.v[i]*m.m[i][j]; return r;
    }
    static bool Finite(const Mat& m) {
        for(int i=0;i<4;++i) for(int j=0;j<4;++j) if(!std::isfinite(m.m[i][j])) return false;
        return true;
    }
    // Convert camera-to-world Matrix34 explicitly (the engine helper is not used).
    static Mat CameraRows(const Matrix34& camera) {
        Mat m={};
        for(int i=0;i<4;++i) {
            const Vector3 r=camera.GetRow(i);
            m.m[i][0]=r.X; m.m[i][1]=r.Y; m.m[i][2]=r.Z;
        }
        m.m[3][3]=1.0f;
        return m;
    }
    static Mat Multiply(const Mat& a,const Mat& b) {
        Mat r={};
        for(int i=0;i<4;++i) for(int j=0;j<4;++j)
            for(int k=0;k<4;++k) r.m[i][j]+=a.m[i][k]*b.m[k][j];
        return r;
    }
    static Mat MotionProjection(const Mat& current,const Matrix44& actual,bool& compensated) {
        compensated=false;
        static const bool enabled=HookConfig::GetBool("LightTrails","CompensateCameraShake",true);
        Matrix34 base34,shaken34;
        if(!enabled || !GetLightTrailShakeMatrices(base34,shaken34)) return current;
        const Mat base=CameraRows(base34), shaken=CameraRows(shaken34);
        Mat active; static_assert(sizeof(Matrix44)==sizeof(Mat),"Camera matrix layout");
        std::memcpy(&active,&actual,sizeof(active));
        if(!Finite(base)||!Finite(shaken)||!Finite(active)) return current;
        // Do not apply player camera compensation to unrelated render passes.
        for(int i=0;i<4;++i) for(int j=0;j<3;++j)
            if(std::fabs(active.m[i][j]-shaken.m[i][j])>(i==3?0.02f:0.0002f)) return current;
        // Row-vector convention: VP_base = inverse(C_base) * C_shaken * VP_render.
        // Both camera transforms are rigid; compute the inverse without touching D3D.
        Mat inv={}; inv.m[3][3]=1.0f;
        for(int i=0;i<3;++i) for(int j=0;j<3;++j) inv.m[i][j]=base.m[j][i];
        for(int j=0;j<3;++j) for(int k=0;k<3;++k) inv.m[3][j]-=base.m[3][k]*inv.m[k][j];
        const Mat result=Multiply(Multiply(inv,shaken),current);
        if(!Finite(result)) return current;
        compensated=true;
        return result;
    }
    using LightKey=std::array<float,3>;
    struct Motion { float x,y; };
    using MotionCache=std::map<LightKey,Motion>;
    static MotionCache& Motions() { static MotionCache cache; return cache; }
    static float Smooth(float x) { x=Clamp(x,0.0f,1.0f); return x*x*(3.0f-2.0f*x); }
    static void Trace(int reason,const char* msg,int count) {
        static const bool enabled=HookConfig::GetBool("LightTrails","DebugTrails",false);
        if(!enabled) return;
        static ULONGLONG last[12]={}; const auto now=GetTickCount64();
        if(last[reason] && now-last[reason]<2000) return;
        last[reason]=now; hook_output("[LightTrails v0.3.1] %s (%d)\n",msg,count);
    }
    static bool IsMainTarget(IDirect3DDevice9* dev,const D3DVIEWPORT9& vp) {
        IDirect3DSurface9* rt=nullptr; IDirect3DSurface9* bb=nullptr;
        bool result=false;
        if(SUCCEEDED(dev->GetRenderTarget(0,&rt)) && rt &&
           SUCCEEDED(dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb)) && bb) {
            D3DSURFACE_DESC a={},b={};
            if(SUCCEEDED(rt->GetDesc(&a)) && SUCCEEDED(bb->GetDesc(&b))) {
                static const bool strict=HookConfig::GetBool("LightTrails","MainBackBufferOnly",false);
                result=(rt==bb || (!strict && a.Width==b.Width && a.Height==b.Height)) &&
                       vp.Width==a.Width && vp.Height==a.Height && vp.X==0 && vp.Y==0;
            }
        }
        if(bb) bb->Release(); if(rt) rt->Release(); return result;
    }
    static DWORD Color(DWORD color,float gain) {
        // Native packed diffuse color, with additive RGB and no alpha-channel writes.
        DWORD out=0xff000000u;
        const float alpha=float((color>>24)&255u)/255.0f;
        gain=Clamp(gain*alpha,0.0f,1.0f);
        for(int s=0;s<24;s+=8) out|=DWORD(float((color>>s)&255u)*gain+0.5f)<<s;
        return out;
    }
    static void Ribbon(std::vector<Vertex>& vertices,float x,float y,float z,float rhw,float dx,float dy,float halfWidth,DWORD color,float intensity) {
        const float len=std::sqrt(dx*dx+dy*dy);
        if(len<1.0f || halfWidth<=0.0f || intensity<=0.0f) return;
        const float nx=-dy/len,ny=dx/len;
        // A continuous ribbon with a Gaussian cross-section and smoothly faded ends.
        const int steps=20, sides=12;
        Vertex grid[steps+1][sides+1];
        for(int i=0;i<=steps;++i) {
            const float t=float(i)/steps;
            const float fade=std::exp(-3.5f*t*t)*(1.0f-Smooth((t-0.65f)/0.35f));
            const float width=halfWidth*(1.0f-0.15f*t);
            for(int j=0;j<=sides;++j) {
                const float u=2.0f*float(j)/sides-1.0f;
                const float edge=std::exp(-4.5f*u*u)*(1.0f-Smooth((std::fabs(u)-0.65f)/0.35f));
                const float gain=intensity*fade*edge;
                grid[i][j]={x+dx*t+nx*u*width,y+dy*t+ny*u*width,z,rhw,Color(color,gain)};
            }
        }
        for(int i=0;i<steps;++i) for(int j=0;j<sides;++j) {
            vertices.push_back(grid[i][j]); vertices.push_back(grid[i+1][j]); vertices.push_back(grid[i][j+1]);
            vertices.push_back(grid[i][j+1]); vertices.push_back(grid[i+1][j]); vertices.push_back(grid[i+1][j+1]);
        }
    }
    static HRESULT Render(IDirect3DDevice9* dev,const std::vector<Vertex>& vertices) {
        if(vertices.empty()) return S_FALSE;
        IDirect3DStateBlock9* state=nullptr;
        HRESULT hr=dev->CreateStateBlock(D3DSBT_ALL,&state);
        if(FAILED(hr) || !state) return FAILED(hr)?hr:E_FAIL;
        // DrawPrimitiveUP clears stream 0: preserve it explicitly as well.
        IDirect3DVertexBuffer9* stream=nullptr; UINT offset=0,stride=0;
        hr=dev->GetStreamSource(0,&stream,&offset,&stride);
        if(FAILED(hr)) { state->Release(); return hr; }
        DWORD nativeZ=D3DZB_TRUE, nativeFunc=D3DCMP_LESSEQUAL;
        dev->GetRenderState(D3DRS_ZENABLE,&nativeZ);
        dev->GetRenderState(D3DRS_ZFUNC,&nativeFunc);
        bool ok=true;
        #define LT_SET(call) do { if(FAILED(dev->call)) ok=false; } while(0)
        LT_SET(SetVertexShader(nullptr)); LT_SET(SetPixelShader(nullptr));
        LT_SET(SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE));
        LT_SET(SetTexture(0,nullptr));
        LT_SET(SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG1));
        LT_SET(SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_DIFFUSE));
        LT_SET(SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG1));
        LT_SET(SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_DIFFUSE));
        LT_SET(SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));
        LT_SET(SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE));
        LT_SET(SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ONE));
        LT_SET(SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE));
        LT_SET(SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD));
        LT_SET(SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_ALPHATESTENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE));
        LT_SET(SetRenderState(D3DRS_FILLMODE,D3DFILL_SOLID));
        LT_SET(SetRenderState(D3DRS_FOGENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_LIGHTING,FALSE));
        LT_SET(SetRenderState(D3DRS_STENCILENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_SHADEMODE,D3DSHADE_GOURAUD));
        LT_SET(SetRenderState(D3DRS_COLORWRITEENABLE,D3DCOLORWRITEENABLE_RED|D3DCOLORWRITEENABLE_GREEN|D3DCOLORWRITEENABLE_BLUE));
        static const bool depth=HookConfig::GetBool("LightTrails","DepthTest",true);
        LT_SET(SetRenderState(D3DRS_ZENABLE,depth?(nativeZ==D3DZB_FALSE?D3DZB_TRUE:nativeZ):D3DZB_FALSE));
        LT_SET(SetRenderState(D3DRS_ZWRITEENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_ZFUNC,nativeFunc));
        LT_SET(SetRenderState(D3DRS_DEPTHBIAS,0));
        LT_SET(SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,0));
        #undef LT_SET
        hr=ok?dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,UINT(vertices.size()/3),vertices.data(),sizeof(Vertex)):E_FAIL;
        const HRESULT restored=state->Apply(); state->Release();
        const HRESULT streamRestored=dev->SetStreamSource(0,stream,offset,stride);
        if(stream) stream->Release();
        if(FAILED(restored)) return restored;
        if(FAILED(streamRestored)) return streamRestored;
        return hr;
    }
    // Same level sweep ABI used by the validated orbit camera, with a small
    // endpoint margin to avoid counting the light's own mounting surface.
    struct SweepResult { unsigned char prefix[0x20]; float remaining; unsigned char suffix[0x1c]; };
    static_assert(sizeof(SweepResult)==0x40,"Sweep buffer size");
    using SweepFn=bool (__thiscall*)(phLevel*,Vector3*,Vector3*,float,SweepResult*,int,int,int);
    static int PlayerIgnore() {
        auto* manager=mcPlayerManager::Instance.get();
        if(!manager || manager->m_NumPlayers<1 || !manager->m_Players[0] || !manager->m_Players[0]->m_Entity) return 0;
        auto* sim=manager->m_Players[0]->m_Entity->m_Car.m_CarSim;
        if(!sim || !sim->m_Collider || !sim->m_Collider->m_SomeInstParent) return 0;
        return reinterpret_cast<int>(sim->m_Collider->m_SomeInstParent->m_Archetype);
    }
    static bool SourceVisible(const Vector3& camera,const Sprite& s,int ignore) {
        static const bool enabled=HookConfig::GetBool("LightTrails","SourceOcclusion",true);
        if(!enabled) return true;
        auto* level=phLevel::Instance.get();
        if(!level) return false;
        Vector3 end(s.x,s.y,s.z), delta=end-camera;
        const float distance=delta.Mag();
        static const float maxDistance=Setting("OcclusionMaxDistance",180.0f,10.0f,350.0f);
        if(!std::isfinite(distance)||distance>maxDistance||distance<0.8f) return false;
        void** vt=*reinterpret_cast<void***>(level);
        if(!vt || !vt[0xB8/sizeof(void*)]) return false;
        static const float margin=Setting("OcclusionEndMargin",0.45f,0.05f,1.0f);
        Vector3 start=camera+delta*(0.10f/distance);
        end=camera+delta*((distance-margin)/distance);
        SweepResult hit={};
        return !reinterpret_cast<SweepFn>(vt[0xB8/sizeof(void*)])(level,&start,&end,0.02f,&hit,ignore,0x1F,1);
    }
    static void __cdecl Draw(int mode,const Sprite* sprites,const Meta* meta,int count) {
        reinterpret_cast<DrawFn>(0x5FB7E0)(mode,sprites,meta,count);
        auto& h=State();
        auto* game=mcGameState::Instance.get();
        if(!game || game->m_CurrentState!=GameState::Game || game->m_IsPaused || game->m_IsPausedLocally || datTimeManager::SpeedrunMode) { h.valid=false; Motions().clear(); return; }
        if(mode!=1) return; // Do not invalidate the main view for an unrelated pass.
        auto* dev=gfxState::lpD3DDevice.get(); D3DVIEWPORT9 vp={};
        if(!dev || FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height) return;
        if(!IsMainTarget(dev,vp)) { Trace(0,"Skipped non-main render target",count); return; }
        const unsigned int frame=Timer::s_LastFrameTick.get();
        if(h.valid && frame==h.frame) return;
        Mat current; std::memcpy(&current,reinterpret_cast<const void*>(0x858480),sizeof(current));
        if(!Finite(current) || !sprites || count<=0 || count>256) return;
        LARGE_INTEGER clock={},freq={};
        if(!QueryPerformanceCounter(&clock)||!QueryPerformanceFrequency(&freq)||freq.QuadPart<=0) return;
        const auto now=clock.QuadPart;
        const double elapsed=h.valid?double(now-h.time)/double(freq.QuadPart):0.0;
        const bool ready=h.valid && elapsed>0.0 && elapsed<=0.5 && h.width==vp.Width && h.height==vp.Height && h.x==vp.X && h.y==vp.Y;
        const float dt=ready?float(elapsed):0.0f;
        const Matrix44 cameraMatrix=gfxState::GetCameraMatrix();
        bool compensated=false;
        const Mat motionCurrent=MotionProjection(current,cameraMatrix,compensated);
        if(compensated) Trace(6,"Camera shake compensation active",count);
        const Mat previous=h.previous;
        h.previous=motionCurrent; h.time=now; h.frame=frame; h.width=vp.Width; h.height=vp.Height; h.x=vp.X; h.y=vp.Y; h.valid=true;
        if(!ready) { Motions().clear(); Trace(1,"History warming up or empty batch",count); return; }
        static const float length=Setting("TrailLength",0.08f,0.0f,0.25f);
        static const float intensity=Setting("TrailIntensity",0.75f,0.0f,1.0f);
        static const float maxLength=Setting("MaxTrailLength",0.10f,0.0f,0.30f);
        static const float width=Setting("TrailWidth",0.5f,0.05f,3.0f);
        static const float threshold=Setting("MotionThreshold",0.001f,0.0f,0.05f);
        if(length<=0 || intensity<=0 || maxLength<=0) return;
        // Same view-projection convention used by the native glowstar renderer.
        const float sx=0.5f*vp.Width,sy=0.5f*vp.Height;
        const float halfWidth=12.0f*width*(float(vp.Height)/1080.0f);
        const Vector3 camera(cameraMatrix.m30,cameraMatrix.m31,cameraMatrix.m32);
        const int ignore=PlayerIgnore();
        static const float smoothing=Setting("MotionSmoothing",12.0f,0.0f,60.0f);
        const float response=smoothing>0.0f?1.0f-std::exp(-smoothing*dt):1.0f;
        MotionCache nextMotion;
        auto& previousMotion=Motions();
        int occluded=0;
        std::vector<Vertex> vertices; vertices.reserve(size_t(count)*20*12*6);
        int drawn=0;
        for(int i=0;i<count;++i) {
            const Sprite& s=sprites[i]; Vec p={{s.x,s.y,s.z,1.0f}};
            const Vec c=Mul(p,current), stable=Mul(p,motionCurrent), old=Mul(p,previous);
            if(!(c.v[3]>0.01f && stable.v[3]>0.01f && old.v[3]>0.01f)) continue;
            const float x=c.v[0]/c.v[3],y=c.v[1]/c.v[3],z=c.v[2]/c.v[3];
            const float dx=old.v[0]/old.v[3]-stable.v[0]/stable.v[3];
            const float dy=old.v[1]/old.v[3]-stable.v[1]/stable.v[3];
            const float motion=std::sqrt(dx*dx+dy*dy);
            if(!std::isfinite(motion)||!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||z<0.0f||z>1.0f||std::fabs(x)>1.0f||std::fabs(y)>1.0f||motion>0.30f) continue;
            if(!std::isfinite(s.x)||!std::isfinite(s.y)||!std::isfinite(s.z)) continue;
            // Smooth per stationary world-space light, in screen units per second.
            // Cache only the immediately preceding frame to avoid stale trails.
            const LightKey key={{s.x,s.y,s.z}};
            Motion velocity={dx/dt,dy/dt};
            const auto prior=previousMotion.find(key);
            if(prior!=previousMotion.end()) {
                velocity.x=prior->second.x+(velocity.x-prior->second.x)*response;
                velocity.y=prior->second.y+(velocity.y-prior->second.y)*response;
            }
            if(motion<=1e-7f) velocity=Motion{0.0f,0.0f};
            nextMotion[key]=velocity;
            const float magnitude=std::sqrt(velocity.x*velocity.x+velocity.y*velocity.y);
            const float speed=magnitude/60.0f;
            if(!std::isfinite(magnitude)||speed<=threshold||magnitude<=1e-7f) continue;
            const float fade=threshold>0.0f?Smooth((speed-threshold)/threshold):1.0f;
            float scale=length; if(magnitude*scale>maxLength) scale=maxLength/magnitude;
            const float px=vp.X+sx*(1.0f+x)-0.5f,py=vp.Y+sy*(1.0f-y)-0.5f;
            const float tx=velocity.x*scale*sx,ty=-velocity.y*scale*sy;
            // No overlap flicker from sub-pixel streaks: blend in over 1-4 pixels.
            const float pixelFade=Smooth((std::sqrt(tx*tx+ty*ty)-1.0f)/3.0f);
            if(pixelFade<=0.0f) continue;
            if(!SourceVisible(camera,s,ignore)) { ++occluded; continue; }
            const size_t before=vertices.size();
            // Match the native XYZRHW sprite depth and reciprocal homogeneous W.
            Ribbon(vertices,px,py,z,1.0f/c.v[3],tx,ty,halfWidth,s.color,intensity*fade*pixelFade);
            if(vertices.size()!=before) ++drawn;
        }
        previousMotion.swap(nextMotion);
        if(occluded) Trace(5,"Sources occluded or outside sweep range",occluded);
        if(vertices.empty()) { Trace(2,"No qualifying moving lights",count); return; }
        const HRESULT hr=Render(dev,vertices);
        if(FAILED(hr)) Trace(3,"Direct3D draw/state error",int(hr));
        else Trace(4,"Continuous streaks submitted",drawn);
    }
public:
    static void Install() {
        if(!HookConfig::GetBool("LightTrails","EnableLightTrails",false)) return;
        if(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))!=0x400000) return;
        const unsigned char expected[]={0xE8,0xD6,0xCF,0x0D,0x00};
        if(std::memcmp(reinterpret_cast<const void*>(0x51E805),expected,sizeof(expected))!=0) { hook_output("[LightTrails v0.3.1] Unsupported or already hooked call site.\n"); return; }
        InstallCallback("LightTrailHandler::Draw","Continuous light streaks",&Draw,{cb::call(0x51E805)});
        hook_output("[LightTrails v0.3.1] Continuous additive streak hook installed.\n");
    }
};
