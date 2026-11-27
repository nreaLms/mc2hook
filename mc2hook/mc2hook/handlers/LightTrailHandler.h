#pragma once
#include <mc2hook/mc2hook.h>
#include <age/age.h>
#include <age/state/gamestate.h>
#include <age/types.h>
#include <age/data/timemgr.h>
#include <age/gfx/rstate.h>
#include <d3d9.h>
#include <cmath>
#include <cstring>
#include <vector>
#include <array>
#include <map>
#include <deque>
#include "LiveEffectSettings.h"

// Implemented by the Orbit Camera v0.4 compatibility bridge.
bool GetLightTrailShakeMatrices(Matrix34& base, Matrix34& shaken);

// Temporal accumulation of projected glowstar light footprints.
// Addresses validated against the supplied MC2 executable; no sprite duplication.
class LightTrailHandler {
    struct Sprite { float x, y, z, size; unsigned int color; };
    struct Meta { unsigned char a, b; };
    struct Mat { float m[4][4]; };
    struct Vec { float v[4]; };
    struct Vertex { float x,y,z,rhw; DWORD color; };
    static_assert(sizeof(Sprite)==20 && sizeof(Meta)==2, "Unexpected native layout");
    using DrawFn=void (__cdecl*)(int,const Sprite*,const Meta*,int);
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

    template<class T> static void Release(T*& p) { if(p) { p->Release(); p=nullptr; } }
    struct MotionSample { Mat projection; double time; };
    struct Buffers {
        IDirect3DDevice9* device=nullptr;
        IDirect3DTexture9* tex[2]={};
        DWORD width=0,height=0;
        int front=0;
        bool valid=false, seen=false;
        unsigned int frame=0;
        LONGLONG time=0;
        Mat previous={};
        float idle=0.0f;
        float extinctionClock=0.0f;
        std::deque<MotionSample> reverseHistory;
        double reverseTime=0.0;
        void Clear() {
            for(auto& t:tex) LightTrailHandler::Release(t);
            reverseHistory.clear(); reverseTime=0.0;
            device=nullptr; width=height=0; valid=seen=false; front=0; idle=0.0f; extinctionClock=0.0f;
        }
    };
    static Buffers& State() { static Buffers b; return b; }
    using ResetFn=HRESULT (WINAPI*)(IDirect3DDevice9*,D3DPRESENT_PARAMETERS*);
    static ResetFn& OriginalReset() { static ResetFn f=nullptr; return f; }
    static HRESULT WINAPI Reset(IDirect3DDevice9* dev,D3DPRESENT_PARAMETERS* p) {
        State().Clear(); return OriginalReset()(dev,p);
    }
    static bool InstallReset(IDirect3DDevice9* dev) {
        void** table=*reinterpret_cast<void***>(dev);
        if(table[16]==reinterpret_cast<void*>(&Reset)) return true;
        if(OriginalReset()) return false; // Do not chain an unknown second device implementation.
        DWORD prot=0,unused=0;
        if(!VirtualProtect(&table[16],sizeof(void*),PAGE_READWRITE,&prot)) return false;
        OriginalReset()=reinterpret_cast<ResetFn>(table[16]);
        table[16]=reinterpret_cast<void*>(&Reset);
        VirtualProtect(&table[16],sizeof(void*),prot,&unused);
        return true;
    }
    static void Trace(const char* msg,HRESULT code=0) {
        if(!HookConfig::GetBool("LightTrails","DebugTrails",false)) return;
        static ULONGLONG last=0; const auto now=GetTickCount64();
        if(last && now-last<1500) return;
        last=now; hook_output("[LightTrails v0.6.5] %s (0x%08X)\n",msg,unsigned(code));
    }
    static bool Ensure(IDirect3DDevice9* dev,DWORD w,DWORD h) {
        auto& b=State();
        if(b.device==dev && b.width==w && b.height==h && b.tex[0] && b.tex[1]) return true;
        b.Clear();
        if(!InstallReset(dev)) return false;
        for(auto& t:b.tex) {
            if(FAILED(dev->CreateTexture(w,h,1,D3DUSAGE_RENDERTARGET,D3DFMT_A8R8G8B8,D3DPOOL_DEFAULT,&t,nullptr))) {
                b.Clear(); return false;
            }
        }
        b.device=dev; b.width=w; b.height=h; return true;
    }
    struct QuadVertex { float x,y,z,rhw; DWORD color; float u,v; };
    static DWORD Tint(float gain) {
        DWORD c=DWORD(Clamp(gain,0,1)*255); return (c<<24)|(c<<16)|(c<<8)|c;
    }
    static DWORD LightColor(DWORD c,float gain) {
        gain=Clamp(gain*float((c>>24)&255)/255.0f,0,1);
        DWORD r=DWORD(float((c>>16)&255)*gain+0.5f),g=DWORD(float((c>>8)&255)*gain+0.5f),b=DWORD(float(c&255)*gain+0.5f);
        const DWORD a=(r>g?(r>b?r:b):(g>b?g:b));
        return (a<<24)|(r<<16)|(g<<8)|b;
    }
    // Capture/restore the device state AND render targets, which state blocks do not save.
    struct Guard {
        IDirect3DDevice9* dev;
        IDirect3DStateBlock9* state=nullptr;
        IDirect3DSurface9* rt=nullptr,*depth=nullptr;
        IDirect3DVertexBuffer9* stream=nullptr;
        UINT offset=0,stride=0;
        D3DVIEWPORT9 vp={};
        bool ready=false;
        explicit Guard(IDirect3DDevice9* d):dev(d) {
            ready=SUCCEEDED(d->CreateStateBlock(D3DSBT_ALL,&state)) && state &&
                SUCCEEDED(d->GetRenderTarget(0,&rt)) && rt &&
                SUCCEEDED(d->GetViewport(&vp)) &&
                SUCCEEDED(d->GetStreamSource(0,&stream,&offset,&stride));
            if(ready) d->GetDepthStencilSurface(&depth);
        }
        bool RestoreTarget() {
            bool ok=SUCCEEDED(dev->SetTexture(0,nullptr));
            if(FAILED(dev->SetDepthStencilSurface(nullptr))) ok=false;
            if(FAILED(dev->SetRenderTarget(0,rt))) ok=false;
            if(FAILED(dev->SetDepthStencilSurface(depth))) ok=false;
            if(FAILED(dev->SetViewport(&vp))) ok=false;
            return ok;
        }
        ~Guard() {
            if(ready) { RestoreTarget(); state->Apply(); dev->SetStreamSource(0,stream,offset,stride); }
            Release(stream); Release(depth); Release(rt); Release(state);
        }
    };
    static bool Setup(IDirect3DDevice9* dev,bool texture,bool additive,bool depth,DWORD zfunc) {
        bool ok=true;
        #define LT_SET(c) do { if(FAILED(dev->c)) ok=false; } while(0)
        LT_SET(SetVertexShader(nullptr)); LT_SET(SetPixelShader(nullptr));
        LT_SET(SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|(texture?D3DFVF_TEX1:0)));
        LT_SET(SetTextureStageState(0,D3DTSS_COLOROP,texture?D3DTOP_MODULATE:D3DTOP_SELECTARG1));
        LT_SET(SetTextureStageState(0,D3DTSS_COLORARG1,texture?D3DTA_TEXTURE:D3DTA_DIFFUSE));
        LT_SET(SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE));
        LT_SET(SetTextureStageState(0,D3DTSS_ALPHAOP,texture?D3DTOP_MODULATE:D3DTOP_SELECTARG1));
        LT_SET(SetTextureStageState(0,D3DTSS_ALPHAARG1,texture?D3DTA_TEXTURE:D3DTA_DIFFUSE));
        LT_SET(SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE));
        LT_SET(SetTextureStageState(0,D3DTSS_TEXCOORDINDEX,0));
        LT_SET(SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE));
        LT_SET(SetTextureStageState(1,D3DTSS_COLOROP,D3DTOP_DISABLE));
        LT_SET(SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP));
        LT_SET(SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP));
        LT_SET(SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_LINEAR));
        LT_SET(SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_LINEAR));
        LT_SET(SetSamplerState(0,D3DSAMP_MIPFILTER,D3DTEXF_NONE));
        LT_SET(SetSamplerState(0,D3DSAMP_SRGBTEXTURE,FALSE));
        LT_SET(SetRenderState(D3DRS_SRGBWRITEENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_ALPHABLENDENABLE,additive));
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
        LT_SET(SetRenderState(D3DRS_SCISSORTESTENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_SHADEMODE,D3DSHADE_GOURAUD));
        LT_SET(SetRenderState(D3DRS_COLORWRITEENABLE,0xf));
        LT_SET(SetRenderState(D3DRS_ZENABLE,depth?D3DZB_TRUE:D3DZB_FALSE));
        LT_SET(SetRenderState(D3DRS_ZWRITEENABLE,FALSE));
        LT_SET(SetRenderState(D3DRS_ZFUNC,zfunc));
        LT_SET(SetRenderState(D3DRS_DEPTHBIAS,0));
        LT_SET(SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS,0));
        #undef LT_SET
        return ok;
    }
    static bool Blit(IDirect3DDevice9* dev,IDirect3DTexture9* tex,DWORD w,DWORD h,float gain,bool additive) {
        if(!Setup(dev,true,additive,false,D3DCMP_ALWAYS) || FAILED(dev->SetTexture(0,tex))) return false;
        // Kill quantized residual pixels on BOTH feedback and composition.
        // RGB and alpha decay together; point sampling avoids feedback diffusion.
        const DWORD cutoff=DWORD(Setting("AlphaThreshold",.025f,0,.25f)*255.0f);
        if(FAILED(dev->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT)) ||
           FAILED(dev->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT)) ||
           FAILED(dev->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE)) ||
           FAILED(dev->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER)) ||
           FAILED(dev->SetRenderState(D3DRS_ALPHAREF,cutoff))) return false;
        if(additive && FAILED(dev->SetRenderState(D3DRS_COLORWRITEENABLE,7))) return false;
        const DWORD c=Tint(gain);
        const float x=float(w)-0.5f,y=float(h)-0.5f;
        QuadVertex v[4]={{-.5f,-.5f,0,1,c,0,0},{x,-.5f,0,1,c,1,0},{-.5f,y,0,1,c,0,1},{x,y,0,1,c,1,1}};
        const HRESULT hr=dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,v,sizeof(QuadVertex));
        dev->SetTexture(0,nullptr); return SUCCEEDED(hr);
    }
    // Quantized UNORM feedback can round a dim pixel back to itself forever.
    // Subtract one or more 8-bit levels at a time, saturating at zero.
    // Apply this only to the private accumulation target, never to the game image.
    static bool Extinguish(IDirect3DDevice9* dev,DWORD w,DWORD h,DWORD levels) {
        if(!levels) return true;
        if(!Setup(dev,false,true,false,D3DCMP_ALWAYS) || FAILED(dev->SetTexture(0,nullptr))) return false;
        if(FAILED(dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_REVSUBTRACT))) return false;
        levels=levels>255?255:levels;
        const DWORD c=(levels<<24)|(levels<<16)|(levels<<8)|levels;
        const float x=float(w)-.5f,y=float(h)-.5f;
        const Vertex q[4]={{-.5f,-.5f,0,1,c},{x,-.5f,0,1,c},{-.5f,y,0,1,c},{x,y,0,1,c}};
        return SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(Vertex)));
    }
    // Clamp the private history only. MIN with ONE/ONE caps each channel.
    // Never apply this operation to the game's render target.
    static bool LimitAccumulation(IDirect3DDevice9* dev,DWORD w,DWORD h,float limit) {
        if(limit>=1.0f) return true;
        D3DCAPS9 caps={};
        if(FAILED(dev->GetDeviceCaps(&caps)) || !(caps.PrimitiveMiscCaps&D3DPMISCCAPS_BLENDOP)) return true;
        if(!Setup(dev,false,true,false,D3DCMP_ALWAYS) || FAILED(dev->SetTexture(0,nullptr))) return false;
        if(FAILED(dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_MIN))) return false;
        const DWORD c=Tint(limit);
        const float x=float(w)-.5f,y=float(h)-.5f;
        const Vertex q[4]={{-.5f,-.5f,0,1,c},{x,-.5f,0,1,c},{-.5f,y,0,1,c},{x,y,0,1,c}};
        return SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,q,sizeof(Vertex)));
    }
    // Scale deposited energy by screen-space travel relative to the footprint.
    // Normalize to 60 Hz so the response does not depend on frame rate.
    static float AdaptiveGain(float distance,float radius,float dt) {
        const float travel=distance/(radius*dt*60.0f);
        const float t=Clamp(travel,0.0f,1.0f);
        return t*t*(3.0f-2.0f*t);
    }
    // Soft isolated light footprint. No elongated geometry or native sprite replay.
    static void Stamp(std::vector<Vertex>& v,float x,float y,float z,float rhw,float radius,DWORD color,float gain) {
        constexpr int sides=16; constexpr float tau=6.28318530718f;
        const Vertex center={x,y,z,rhw,LightColor(color,gain)};
        for(int i=0;i<sides;++i) {
            const float a=tau*i/sides,b=tau*(i+1)/sides;
            const float ca=std::cos(a),sa=std::sin(a),cb=std::cos(b),sb=std::sin(b);
            Vertex p={x+ca*radius*.4f,y+sa*radius*.4f,z,rhw,LightColor(color,gain*.55f)};
            Vertex q={x+cb*radius*.4f,y+sb*radius*.4f,z,rhw,p.color};
            Vertex r={x+ca*radius,y+sa*radius,z,rhw,0u};
            Vertex t={x+cb*radius,y+sb*radius,z,rhw,r.color};
            v.insert(v.end(),{center,p,q,p,r,q,q,r,t});
        }
    }
    static void __cdecl Draw(int mode,const Sprite* sprites,const Meta* meta,int count) {
        reinterpret_cast<DrawFn>(0x5FB7E0)(mode,sprites,meta,count);
        auto& b=State();
        if (!LiveEffectSettings::Get(LiveEffectSettings::LightTrails)) {
            b.valid=b.seen=false; b.reverseHistory.clear(); b.reverseTime=0.0; return;
        }
        auto* game=mcGameState::Instance.get();
        if(!game || game->m_CurrentState!=GameState::Game || game->m_IsPaused || game->m_IsPausedLocally || datTimeManager::SpeedrunMode) { b.valid=b.seen=false; return; }
        if(mode!=1) return;
        auto* dev=gfxState::lpD3DDevice.get(); D3DVIEWPORT9 vp={};
        if(!dev || FAILED(dev->GetViewport(&vp)) || !vp.Width || !vp.Height || !IsMainTarget(dev,vp)) return;
        const unsigned int frame=Timer::s_LastFrameTick.get();
        if(b.seen && b.frame==frame) return;
        Mat current; std::memcpy(&current,reinterpret_cast<const void*>(0x858480),sizeof(current));
        if(!Finite(current) || count<0 || count>256 || (count && !sprites)) return;
        LARGE_INTEGER now={},freq={};
        if(!QueryPerformanceCounter(&now)||!QueryPerformanceFrequency(&freq)||freq.QuadPart<=0) return;
        Guard guard(dev); if(!guard.ready || !guard.depth) { Trace("No usable depth surface"); return; }
        D3DSURFACE_DESC desc={},dd={};
        if(FAILED(guard.rt->GetDesc(&desc)) || FAILED(guard.depth->GetDesc(&dd)) || desc.MultiSampleType!=D3DMULTISAMPLE_NONE || dd.MultiSampleType!=D3DMULTISAMPLE_NONE || dd.Width<vp.Width || dd.Height<vp.Height) { Trace("Unsupported multisampled or incompatible target"); return; }
        if(!Ensure(dev,vp.Width,vp.Height)) { Trace("Cannot allocate accumulation textures or install reset cleanup"); return; }
        const float dt=b.valid?float(double(now.QuadPart-b.time)/double(freq.QuadPart)):0;
        const bool history=b.valid && dt>0 && dt<.25f;
        const float persistence=Setting("TrailPersistence",.15f,.02f,1.0f);
        const float decay=Setting("TrailDecay",2.0f,.1f,10.0f);
        const float intensity=Setting("TrailIntensity",.65f,0,1);
        const float effectiveIntensity=intensity * LiveEffectSettings::Scale(LiveEffectSettings::LightTrails);
        const float outputIntensity=effectiveIntensity > 1.0f ? 1.0f : effectiveIntensity;
        const float radius=Setting("FootprintRadius",6.0f,1,32)*vp.Height/1080.0f;
        const float retention=history?std::exp(-decay*dt/persistence):0;
        const float injection=1-retention;
        const float stampGain=Setting("DepositGain",2.5f,.25f,8.0f);
        const bool adaptive=HookConfig::GetBool("LightTrails","AdaptiveDeposit",true);
        const float accumulationLimit=Setting("MaxAccumulation",.85f,.05f,1.0f);
        const int substeps=HookConfig::GetInt("LightTrails","SubframeSteps",12);
        const int maxSteps=substeps<1?1:(substeps>32?32:substeps);
        const float extinction=Setting("ResidualCleanup",90.0f,0.0f,255.0f);
        if(!history) b.extinctionClock=0.0f;
        b.extinctionClock+=history?dt*extinction:0.0f;
        const DWORD eraseLevels=DWORD(b.extinctionClock);
        b.extinctionClock-=float(eraseLevels);
        bool compensated=false;
        const Mat stable=MotionProjection(current,gfxState::GetCameraMatrix(),compensated);
        // Stable projection gates new energy from stationary lights. The visible stamp
        // uses actual projection so the native depth buffer stays aligned.
        const bool reverse=HookConfig::GetBool("LightTrails","ReverseTrailDirection",false);
        // A framebuffer history cannot be reflected around each individual light.
        // In reverse mode rebuild the private buffer from recent camera projections.
        if(reverse) {
            if(!history) { b.reverseHistory.clear(); b.reverseTime=0.0; }
            else b.reverseTime+=dt;
            b.reverseHistory.push_back({stable,b.reverseTime});
            const double window=double(persistence)*3.0/double(decay);
            while(b.reverseHistory.size()>2 && (b.reverseTime-b.reverseHistory.front().time>window || b.reverseHistory.size()>64))
                b.reverseHistory.pop_front();
        } else b.reverseHistory.clear();
        const float threshold=Setting("MotionThreshold",.0005f,0,.05f);
        DWORD zfunc=D3DCMP_LESSEQUAL; dev->GetRenderState(D3DRS_ZFUNC,&zfunc);
        const int next=1-b.front;
        IDirect3DSurface9* dst=nullptr;
        if(FAILED(b.tex[next]->GetSurfaceLevel(0,&dst)) || !dst) return;
        bool ok=SUCCEEDED(dev->SetTexture(0,nullptr)) && SUCCEEDED(dev->SetDepthStencilSurface(nullptr)) && SUCCEEDED(dev->SetRenderTarget(0,dst)) && SUCCEEDED(dev->SetViewport(&vp));
        Release(dst);
        if(ok) ok=SUCCEEDED(dev->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));
        if(ok && history && !reverse) ok=Blit(dev,b.tex[b.front],vp.Width,vp.Height,retention,false);
        if(ok && history && !reverse && eraseLevels) {
            D3DCAPS9 caps={};
            if(SUCCEEDED(dev->GetDeviceCaps(&caps)) && (caps.PrimitiveMiscCaps&D3DPMISCCAPS_BLENDOP))
                ok=Extinguish(dev,vp.Width,vp.Height,eraseLevels);
        }
        if(ok) ok=SUCCEEDED(dev->SetDepthStencilSurface(guard.depth));
        std::vector<Vertex> stamps; stamps.reserve(size_t(count)*144*maxSteps);
        unsigned rejectedW=0, rejectedMotion=0, capturedLights=0;
        if(ok && history && reverse) for(int i=0;i<count;++i) {
            const Sprite& s=sprites[i]; const Vec p={{s.x,s.y,s.z,1}};
            const Vec c=Mul(p,current), u=Mul(p,stable);
            if(!(c.v[3]>.01f && u.v[3]>.01f)) { ++rejectedW; continue; }
            const float x=c.v[0]/c.v[3], y=c.v[1]/c.v[3], z=c.v[2]/c.v[3];
            if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||z<0||z>1||std::fabs(x)>1||std::fabs(y)>1) continue;
            const float ux=u.v[0]/u.v[3],uy=u.v[1]/u.v[3];
            const float px=(1+x)*vp.Width*.5f-.5f,py=(1-y)*vp.Height*.5f-.5f;
            bool drawn=false;
            // Reflect each historical offset about the current visible flare.
            // Rebuild using aged deposits, not a second persistent screen buffer.
            for(size_t k=b.reverseHistory.size()-1;k>0;--k) {
                const auto& recent=b.reverseHistory[k]; const auto& older=b.reverseHistory[k-1];
                const float segmentDt=float(recent.time-older.time);
                if(!(segmentDt>0 && segmentDt<.25f)) break;
                const Vec a=Mul(p,recent.projection), d=Mul(p,older.projection);
                if(!(a.v[3]>.01f && d.v[3]>.01f)) break;
                const float ax=a.v[0]/a.v[3],ay=a.v[1]/a.v[3], ox=d.v[0]/d.v[3],oy=d.v[1]/d.v[3];
                const float dx=ax-ox,dy=ay-oy;
                const float motion=std::sqrt(dx*dx+dy*dy);
                if(!std::isfinite(motion)||motion>.3f) break;
                if(motion/segmentDt/60<=threshold) continue;
                const float sx=dx*vp.Width*.5f,sy=-dy*vp.Height*.5f;
                const float distance=std::sqrt(sx*sx+sy*sy);
                const float startX=px+(ux-ax)*vp.Width*.5f,startY=py-(uy-ay)*vp.Height*.5f;
                if(!std::isfinite(startX)||!std::isfinite(startY)||std::fabs(startX-px)>vp.Width||std::fabs(startY-py)>vp.Height) break;
                int steps=int(std::ceil(distance/(radius*.55f)));
                if(steps<1) steps=1; if(steps>maxSteps) steps=maxSteps;
                const float spacing=distance/float(steps);
                const float overlap=spacing>0.001f?Clamp(radius/spacing,1.0f,float(steps)):float(steps);
                const float age=float(b.reverseTime-(recent.time+older.time)*.5);
                const float fade=std::exp(-decay*age/persistence);
                const float gain=Clamp((1-std::exp(-decay*segmentDt/persistence))*stampGain/overlap*(adaptive?AdaptiveGain(distance,radius,segmentDt):1.0f)*fade,0.0f,1.0f);
                if(gain<=0 || !LightColor(s.color,gain)) continue;
                for(int j=0;j<steps;++j) {
                    const float t=(float(j)+.5f)/float(steps);
                    Stamp(stamps,startX+sx*t,startY+sy*t,z,1/c.v[3],radius,s.color,gain);
                }
                drawn=true;
                // Bound CPU geometry even at very high frame rates.
                if(stamps.size()>600000) break;
            }
            if(drawn) ++capturedLights;
            if(stamps.size()>600000) break;
        }
        if(ok && history && !reverse) for(int i=0;i<count;++i) {
            const Sprite& s=sprites[i]; Vec p={{s.x,s.y,s.z,1}};
            const Vec c=Mul(p,current), u=Mul(p,stable), o=Mul(p,b.previous);
            if(!(c.v[3]>.01f && u.v[3]>.01f && o.v[3]>.01f)) { ++rejectedW; continue; }
            const float x=c.v[0]/c.v[3], y=c.v[1]/c.v[3], z=c.v[2]/c.v[3];
            const float dx=u.v[0]/u.v[3]-o.v[0]/o.v[3],dy=u.v[1]/u.v[3]-o.v[1]/o.v[3];
            const float motion=std::sqrt(dx*dx+dy*dy);
            if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z)||!std::isfinite(motion)||z<0||z>1||std::fabs(x)>1||std::fabs(y)>1||motion>.3f||motion/dt/60<=threshold) { ++rejectedMotion; continue; }
            // Interpolate screen motion using the unshaken camera delta; anchor to
            // the actual rendered light. The prior endpoint is an approximation
            // for stationary world lights, not a tracked moving vehicle light.
            const float px=(1+x)*vp.Width*.5f-.5f, py=(1-y)*vp.Height*.5f-.5f;
            const float sx=dx*vp.Width*.5f, sy=-dy*vp.Height*.5f;
            const float distance=std::sqrt(sx*sx+sy*sy);
            int steps=int(std::ceil(distance/(radius*.55f)));
            if(steps<1) steps=1; if(steps>maxSteps) steps=maxSteps;
            const float spacing=distance/float(steps);
            const float overlap=spacing>0.001f?Clamp(radius/spacing,1.0f,float(steps)):float(steps);
            const float motionGain=adaptive?AdaptiveGain(distance,radius,dt):1.0f;
            const float gain=Clamp(injection*stampGain/overlap*motionGain,0.0f,1.0f);
            if(gain<=0.0f || (adaptive && !LightColor(s.color,gain))) continue;
            for(int j=0;j<steps;++j) {
                const float t=(float(j)+.5f)/float(steps);
                Stamp(stamps,px-sx*t,py-sy*t,z,1/c.v[3],radius,s.color,gain);
            }
            ++capturedLights;
        }
        if(ok && !stamps.empty()) {
            ok=Setup(dev,false,true,true,zfunc) && SUCCEEDED(dev->SetTexture(0,nullptr));
            if(ok) ok=SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST,UINT(stamps.size()/3),stamps.data(),sizeof(Vertex)));
        }
        if(ok && !stamps.empty() && accumulationLimit<1.0f)
            ok=LimitAccumulation(dev,vp.Width,vp.Height,accumulationLimit);
        b.idle=(!history || !stamps.empty())?0.0f:b.idle+dt;
        const bool expired=b.idle>persistence*3.0f/decay;
        if(ok && expired) ok=SUCCEEDED(dev->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0));
        // Composite previous history only: no duplicate current flare at its current position.
        if(!guard.RestoreTarget()) ok=false;
        if(ok && history && !expired) ok=Blit(dev,b.tex[reverse?next:b.front],vp.Width,vp.Height,reverse?outputIntensity:outputIntensity*retention,true);
        if(ok) { b.front=next; b.valid=true; b.seen=true; b.frame=frame; b.time=now.QuadPart; b.previous=stable; if(HookConfig::GetBool("LightTrails","DebugTrails",false)) {
            static ULONGLONG lastReport=0; const auto tick=GetTickCount64();
            if(!lastReport || tick-lastReport>=1500) {
                lastReport=tick;
                hook_output("[LightTrails v0.6.5] history=%d batch=%d captured=%u rejectedW=%u rejectedMotion=%u dt=%.4f\n",int(history),count,capturedLights,rejectedW,rejectedMotion,double(dt));
            }
        } }
        else { b.valid=false; Trace("Accumulation pass failed"); }
    }
public:
    static void Install() {
        if(!LiveEffectSettings::MenuEnabled() && !LiveEffectSettings::Get(LiveEffectSettings::LightTrails)) return;
        if(reinterpret_cast<uintptr_t>(GetModuleHandleA(nullptr))!=0x400000) return;
        const unsigned char expected[]={0xE8,0xD6,0xCF,0x0D,0x00};
        if(std::memcmp(reinterpret_cast<const void*>(0x51E805),expected,sizeof(expected))!=0) { hook_output("[LightTrails v0.6.5] Unsupported call site.\n"); return; }
        InstallCallback("LightTrailHandler::Draw","Temporal light accumulation",&Draw,{cb::call(0x51E805)});
        hook_output("[LightTrails v0.6.5] Experimental accumulation hook installed.\n");
    }
};
