/* com.c: Direct3D 9, DirectSound and DirectInput as "null" back ends. The game loads these DLLs with LoadLibrary and talks to
 * them through COM vtables; here every interface is a guest object whose vtable slots are host functions. Resources keep
 * real memory behind Lock() so the game can fill textures and buffers, but nothing is rendered, played or read.
 * This file is the template for a real back end: replace the handlers (Present, Draw*, the sound buffers, the input
 * devices) and keep the object model. */
#define _GNU_SOURCE
#include <time.h>
#include "port.h"

enum { C_D3D9, C_DEV, C_TEX, C_CUBE, C_SURF, C_BUF, C_SWAP, C_QUERY, C_GEN, C_DS, C_DSB, C_DS3B, C_DS3L, C_DI, C_DID, C_DD, C_KSP, C_SB, C_NCLASS };
typedef void (*mfn)(CPU *c, uint32_t self);
typedef struct { const char *name; const char *spec; } IfaceDef;
#define UNK "QueryInterface:2 AddRef:0 Release:0 "
#define RES "GetDevice:1 SetPrivateData:4 GetPrivateData:3 FreePrivateData:1 SetPriority:1 GetPriority:0 PreLoad:0 GetType:0 "
static const IfaceDef ifaces[C_NCLASS] = {
 [C_D3D9] = { "IDirect3D9", UNK "RegisterSoftwareDevice:1 GetAdapterCount:0 GetAdapterIdentifier:3 GetAdapterModeCount:2 EnumAdapterModes:4 GetAdapterDisplayMode:2 CheckDeviceType:5 CheckDeviceFormat:6 CheckDeviceMultiSampleType:6 CheckDepthStencilMatch:5 CheckDeviceFormatConversion:4 GetDeviceCaps:3 GetAdapterMonitor:1 CreateDevice:6" },
 [C_DEV] = { "IDirect3DDevice9", UNK "TestCooperativeLevel:0 GetAvailableTextureMem:0 EvictManagedResources:0 GetDirect3D:1 GetDeviceCaps:1 GetDisplayMode:2 GetCreationParameters:1 SetCursorProperties:3 SetCursorPosition:3 ShowCursor:1 CreateAdditionalSwapChain:2 GetSwapChain:2 GetNumberOfSwapChains:0 Reset:1 Present:4 GetBackBuffer:4 GetRasterStatus:2 SetDialogBoxMode:1 SetGammaRamp:3 GetGammaRamp:2 CreateTexture:8 CreateVolumeTexture:9 CreateCubeTexture:7 CreateVertexBuffer:6 CreateIndexBuffer:6 CreateRenderTarget:8 CreateDepthStencilSurface:8 UpdateSurface:4 UpdateTexture:2 GetRenderTargetData:2 GetFrontBufferData:2 StretchRect:5 ColorFill:3 CreateOffscreenPlainSurface:6 SetRenderTarget:2 GetRenderTarget:2 SetDepthStencilSurface:1 GetDepthStencilSurface:1 BeginScene:0 EndScene:0 Clear:6 SetTransform:2 GetTransform:2 MultiplyTransform:2 SetViewport:1 GetViewport:1 SetMaterial:1 GetMaterial:1 SetLight:2 GetLight:2 LightEnable:2 GetLightEnable:2 SetClipPlane:2 GetClipPlane:2 SetRenderState:2 GetRenderState:2 CreateStateBlock:2 BeginStateBlock:0 EndStateBlock:1 SetClipStatus:1 GetClipStatus:1 GetTexture:2 SetTexture:2 GetTextureStageState:3 SetTextureStageState:3 GetSamplerState:3 SetSamplerState:3 ValidateDevice:1 SetPaletteEntries:2 GetPaletteEntries:2 SetCurrentTexturePalette:1 GetCurrentTexturePalette:1 SetScissorRect:1 GetScissorRect:1 SetSoftwareVertexProcessing:1 GetSoftwareVertexProcessing:0 SetNPatchMode:1 GetNPatchMode:0 DrawPrimitive:3 DrawIndexedPrimitive:6 DrawPrimitiveUP:4 DrawIndexedPrimitiveUP:8 ProcessVertices:5 CreateVertexDeclaration:2 SetVertexDeclaration:1 GetVertexDeclaration:1 SetFVF:1 GetFVF:1 CreateVertexShader:2 SetVertexShader:1 GetVertexShader:1 SetVertexShaderConstantF:3 GetVertexShaderConstantF:3 SetVertexShaderConstantI:3 GetVertexShaderConstantI:3 SetVertexShaderConstantB:3 GetVertexShaderConstantB:3 SetStreamSource:4 GetStreamSource:4 SetStreamSourceFreq:2 GetStreamSourceFreq:2 SetIndices:1 GetIndices:1 CreatePixelShader:2 SetPixelShader:1 GetPixelShader:1 SetPixelShaderConstantF:3 GetPixelShaderConstantF:3 SetPixelShaderConstantI:3 GetPixelShaderConstantI:3 SetPixelShaderConstantB:3 GetPixelShaderConstantB:3 DrawRectPatch:3 DrawTriPatch:3 DeletePatch:1 CreateQuery:2" },
 [C_TEX] = { "IDirect3DTexture9", UNK RES "SetLOD:1 GetLOD:0 GetLevelCount:0 SetAutoGenFilterType:1 GetAutoGenFilterType:0 GenerateMipSubLevels:0 GetLevelDesc:2 GetSurfaceLevel:2 LockRect:4 UnlockRect:1 AddDirtyRect:1" },
 [C_CUBE] = { "IDirect3DCubeTexture9", UNK RES "SetLOD:1 GetLOD:0 GetLevelCount:0 SetAutoGenFilterType:1 GetAutoGenFilterType:0 GenerateMipSubLevels:0 GetLevelDesc:2 GetCubeMapSurface:3 LockRect:5 UnlockRect:2 AddDirtyRect:2" },
 [C_SURF] = { "IDirect3DSurface9", UNK RES "GetContainer:2 GetDesc:1 LockRect:3 UnlockRect:0 GetDC:1 ReleaseDC:1" },
 [C_BUF] = { "IDirect3DBuffer9", UNK RES "Lock:4 Unlock:0 GetDesc:1" },
 [C_SWAP] = { "IDirect3DSwapChain9", UNK "Present:5 GetFrontBufferData:1 GetBackBuffer:3 GetRasterStatus:1 GetDisplayMode:1 GetDevice:1 GetPresentParameters:1" },
 [C_QUERY] = { "IDirect3DQuery9", UNK "GetDevice:1 GetType:0 GetDataSize:0 Issue:1 GetData:3" },
 [C_GEN] = { "IDirect3DResource9", UNK "GetDevice:1 GetDeclaration:2 GetFunction:2 Capture:0 Apply:0 SetPrivateData:4 GetPrivateData:3 FreePrivateData:1 SetPriority:1 GetPriority:0 PreLoad:0 GetType:0 Slot21:0 Slot22:0 Slot23:0 Slot24:0 Slot25:0 Slot26:0 Slot27:0 Slot28:0 Slot29:0 Slot30:0 Slot31:0 Slot32:0" },
 [C_DS] = { "IDirectSound", UNK "CreateSoundBuffer:3 GetCaps:1 DuplicateSoundBuffer:2 SetCooperativeLevel:2 Compact:0 GetSpeakerConfig:1 SetSpeakerConfig:1 Initialize:1" },
 [C_DSB] = { "IDirectSoundBuffer", UNK "GetCaps:1 GetCurrentPosition:2 GetFormat:3 GetVolume:1 GetPan:1 GetFrequency:1 GetStatus:1 Initialize:2 Lock:7 Play:3 SetCurrentPosition:1 SetFormat:1 SetVolume:1 SetPan:1 SetFrequency:1 Stop:0 Unlock:4 Restore:0" },
 [C_DS3B] = { "IDirectSound3DBuffer", UNK "GetAllParameters:1 GetConeAngles:2 GetConeOrientation:1 GetConeOutsideVolume:1 GetMaxDistance:1 GetMinDistance:1 GetMode:1 GetPosition:1 GetVelocity:1 SetAllParameters:2 SetConeAngles:3 SetConeOrientation:4 SetConeOutsideVolume:2 SetMaxDistance:2 SetMinDistance:2 SetMode:2 SetPosition:4 SetVelocity:4" },
 [C_DS3L] = { "IDirectSound3DListener", UNK "GetAllParameters:1 GetDistanceFactor:1 GetDopplerFactor:1 GetOrientation:2 GetPosition:1 GetRolloffFactor:1 SetAllParameters:2 SetDistanceFactor:2 SetDopplerFactor:2 SetOrientation:7 SetPosition:4 SetRolloffFactor:2 CommitDeferredSettings:0" },
 [C_DI] = { "IDirectInput", UNK "CreateDevice:3 EnumDevices:4 GetDeviceStatus:1 RunControlPanel:2 Initialize:3" },
 [C_DD] = { "IDirectDraw", UNK "Compact:0 CreateClipper:3 CreatePalette:4 CreateSurface:3 DuplicateSurface:2 EnumDisplayModes:4 EnumSurfaces:4 FlipToGDISurface:0 GetCaps:2 GetDisplayMode:1 GetFourCCCodes:2 GetGDISurface:1 GetMonitorFrequency:1 GetScanLine:1 GetVerticalBlankStatus:1 Initialize:1 RestoreDisplayMode:0 SetCooperativeLevel:2 SetDisplayMode:3 WaitForVerticalBlank:2" },
 [C_SB] = { "IDirect3DStateBlock9", UNK "GetDevice:1 Capture:0 Apply:0" },
 [C_KSP] = { "IKsPropertySet", UNK "Get:7 Set:6 QuerySupport:3" },
 [C_DID] = { "IDirectInputDevice", UNK "GetCapabilities:1 EnumObjects:3 GetProperty:2 SetProperty:2 Acquire:0 Unacquire:0 GetDeviceState:2 GetDeviceData:4 SetDataFormat:1 SetEventNotification:1 SetCooperativeLevel:2 GetObjectInfo:3 GetDeviceInfo:1 RunControlPanel:2 Initialize:3" },
};
typedef struct { int cls; const char *name; mfn fn; } Handler;
static const Handler *handlers_tab; static int nhandlers;     /* filled at the end of this file */
static uint32_t vtable[C_NCLASS]; static mfn vt_fn[C_NCLASS][160];

/* per-object host state */
typedef struct { uint32_t w, h, fmt, levels, size, data, lvl[16], parent, level; uint32_t playing, looping, loops_start_ms, avg, wfx, flags; } Obj;
static Obj **objs; static unsigned nobjs;
static Obj *O(uint32_t self) { return objs[RD32(self + 12)]; }
static uint32_t com_new(int cls) {
    uint32_t o = g_alloc(16); WR32(o, vtable[cls]); WR32(o + 4, 1); WR32(o + 8, (uint32_t)cls);
    objs = realloc(objs, (nobjs + 1) * sizeof *objs); objs[nobjs] = calloc(1, sizeof(Obj)); WR32(o + 12, nobjs++); return o;
}
static uint32_t ms_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u); }
#define OUT(i, v) do { uint32_t p_ = A(i); if (p_) WR32(p_, (v)); } while (0)

static void com_dispatch(CPU *c) {
    int cls = (int)(g_targ >> 8), slot = (int)(g_targ & 255); uint32_t self = A(0);
    mfn f = vt_fn[cls][slot];
    if (f) { c->eax = 0; f(c, self); return; }
    c->eax = 0;     /* S_OK; outputs (if any) stay as the game initialised them */
}
static void build_vtable(int cls) {
    if (vtable[cls]) return;
    char spec[8192]; snprintf(spec, sizeof spec, "%s", ifaces[cls].spec); int n = 0; char *save, *tok;
    uint32_t addrs[160];
    for (tok = strtok_r(spec, " ", &save); tok && n < 160; tok = strtok_r(NULL, " ", &save), n++) {
        char *colon = strchr(tok, ':'); *colon = 0; int nargs = atoi(colon + 1);
        char full[96]; snprintf(full, sizeof full, "%s::%s", ifaces[cls].name, tok);
        addrs[n] = g_thunk_arg(com_dispatch, 4 * (nargs + 1), strdup(full), ((uint32_t)cls << 8) | (uint32_t)n);
        for (int h = 0; h < nhandlers; h++) if (handlers_tab[h].cls == cls && !strcmp(handlers_tab[h].name, tok)) vt_fn[cls][n] = handlers_tab[h].fn;
    }
    uint32_t vt = g_alloc(4 * (uint32_t)n); for (int i = 0; i < n; i++) WR32(vt + 4 * (uint32_t)i, addrs[i]); vtable[cls] = vt;
}
static uint32_t make(int cls) { build_vtable(cls); return com_new(cls); }

/* ---------------------------------------------------------------- IUnknown for every class */
static void h_addref(CPU *c, uint32_t s) { WR32(s + 4, RD32(s + 4) + 1); c->eax = RD32(s + 4); }
static void h_release(CPU *c, uint32_t s) { uint32_t r = RD32(s + 4); if (r) WR32(s + 4, --r); c->eax = r; }
static void h_qi(CPU *c, uint32_t s) {
    uint32_t iid0 = A(1) ? RD32(A(1)) : 0; int cls = (int)RD32(s + 8); uint32_t r = s;
    if (cls == C_DSB || cls == C_DS3B || cls == C_DS3L || cls == C_DS || cls == C_KSP) {
        /* only the interfaces we really implement; anything else (IDirectSoundBuffer8, EAX property sets, ...) is refused, so the
           game falls back instead of calling a vtable of the wrong shape */
        if (iid0 == 0x279AFA86u) r = make(C_DS3B); else if (iid0 == 0x279AFA84u) r = make(C_DS3L); else if (iid0 == 0x31EFAC30u) r = make(C_KSP);
        else if (iid0 == 0x279AFA85u || iid0 == 0x279AFA83u || iid0 == 0) WR32(s + 4, RD32(s + 4) + 1);
        else { OUT(2, 0); c->eax = 0x80004002u; return; }
    } else WR32(s + 4, RD32(s + 4) + 1);
    OUT(2, r); c->eax = 0;
}
static void ksp_query(CPU *c, uint32_t s) { (void)s; OUT(3, 0); }       /* no EAX/property support */
static void ksp_fail(CPU *c, uint32_t s) { (void)s; c->eax = 0x80070490u; }

/* ---------------------------------------------------------------- Direct3D 9 */
static void fill_caps(uint32_t p) {
    memset(GP(p), 0xFF, 88); WR32(p, 1); WR32(p + 4, 0);                 /* HAL, adapter 0, every capability bit set */
    memset(GP(p + 88), 0, 304 - 88);
    WR32(p + 88, 4096); WR32(p + 92, 4096); WR32(p + 96, 0x8000); WR32(p + 100, 8192); WR32(p + 104, 4096); WR32(p + 108, 16); WR32(p + 112, 0x501502F9u);
    WR32(p + 136, 0xFF); WR32(p + 140, 0x0008FFFFu); WR32(p + 144, 0xFFFFFFFFu); WR32(p + 148, 8); WR32(p + 152, 8); WR32(p + 156, 0xFFFFFFFFu); WR32(p + 160, 8); WR32(p + 164, 6);
    WR32(p + 168, 4); WR32(p + 172, 0); WR32(p + 176, 0x43800000u); WR32(p + 180, 0xFFFFF); WR32(p + 184, 0xFFFFF); WR32(p + 188, 16); WR32(p + 192, 255);
    WR32(p + 196, 0xFFFE0300u); WR32(p + 200, 256); WR32(p + 204, 0xFFFF0300u); WR32(p + 208, 0x501502F9u);
}
static void put_mode(uint32_t p, uint32_t w, uint32_t h) { WR32(p, w); WR32(p + 4, h); WR32(p + 8, 60); WR32(p + 12, 22); }
static const uint32_t modes[4][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1920, 1080 } };
static void d3d_adapter_id(CPU *c, uint32_t s) { uint32_t p = A(3); memset(GP(p), 0, 1100); strcpy((char *)GP(p), "nullgfx"); strcpy((char *)GP(p + 512), "Portable null renderer"); strcpy((char *)GP(p + 1024), "\\\\.\\DISPLAY1"); (void)s; }
static void d3d_modecount(CPU *c, uint32_t s) { (void)s; c->eax = 4; }
static void d3d_enummodes(CPU *c, uint32_t s) { (void)s; uint32_t m = A(3); if (m >= 4) { c->eax = 0x8876086Cu; return; } put_mode(A(4), modes[m][0], modes[m][1]); }
static void d3d_dispmode(CPU *c, uint32_t s) { (void)s; put_mode(A(2), 1920, 1080); }
static void d3d_adaptercount(CPU *c, uint32_t s) { (void)s; c->eax = 1; }
static void d3d_msaa(CPU *c, uint32_t s) { (void)s; if (A(5) > 1) { c->eax = 0x8876086Au; return; } OUT(6, 1); }
static void d3d_getcaps(CPU *c, uint32_t s) { (void)c; (void)s; fill_caps(A(3)); }
static void d3d_monitor(CPU *c, uint32_t s) { (void)s; c->eax = 1; }
static void d3d_createdevice(CPU *c, uint32_t s) {
    uint32_t pp = A(5); uint32_t w = RD32(pp), h = RD32(pp + 4); if (!w || !h) { w = g_ww; h = g_wh; }
    uint32_t d = make(C_DEV); Obj *o = O(d); o->w = w; o->h = h; o->fmt = RD32(pp + 8) ? RD32(pp + 8) : 22; (void)s;
    port_log("Direct3D9: CreateDevice %ux%u (null renderer: nothing is drawn)", w, h); OUT(6, d);
}
static void fmt_info(uint32_t fmt, uint32_t w, uint32_t h, uint32_t *pitch, uint32_t *size) {
    uint32_t bw = (w + 3) / 4 ? (w + 3) / 4 : 1, bh = (h + 3) / 4 ? (h + 3) / 4 : 1;
    switch (fmt) {
    case 0x31545844: *pitch = bw * 8; *size = *pitch * bh; return;                       /* DXT1 */
    case 0x33545844: case 0x35545844: *pitch = bw * 16; *size = *pitch * bh; return;     /* DXT3 / DXT5 */
    case 23: case 25: case 26: case 30: case 31: case 80: case 81: *pitch = w * 2; break;
    case 28: case 50: case 41: *pitch = w; break;
    case 113: *pitch = w * 8; break; case 116: *pitch = w * 16; break;
    default: *pitch = w * 4; break;
    }
    *size = *pitch * h;
}
static uint32_t lvl_data(Obj *o, uint32_t l, uint32_t *pitch) {
    if (l > 15) l = 15; uint32_t w = o->w >> l, h = o->h >> l; if (!w) w = 1; if (!h) h = 1; uint32_t sz; fmt_info(o->fmt, w, h, pitch, &sz);
    if (!o->lvl[l]) o->lvl[l] = g_alloc(sz ? sz : 4); return o->lvl[l];
}
static void dev_createtexture(CPU *c, uint32_t s) {
    (void)s; uint32_t t = make(C_TEX); Obj *o = O(t); o->w = A(1); o->h = A(2); o->levels = A(3); o->fmt = A(5);
    if (!o->levels) { uint32_t m = o->w > o->h ? o->w : o->h; while (m) { o->levels++; m >>= 1; } } OUT(7, t);
}
static void dev_createcube(CPU *c, uint32_t s) { (void)s; uint32_t t = make(C_CUBE); Obj *o = O(t); o->w = o->h = A(1); o->levels = A(2) ? A(2) : 1; o->fmt = A(4); OUT(6, t); }
static void dev_createvb(CPU *c, uint32_t s) { (void)s; uint32_t b = make(C_BUF); Obj *o = O(b); o->size = A(1); o->data = g_alloc(A(1) ? A(1) : 4); o->flags = A(2); OUT(5, b); }
static void dev_createib(CPU *c, uint32_t s) { (void)s; uint32_t b = make(C_BUF); Obj *o = O(b); o->size = A(1); o->data = g_alloc(A(1) ? A(1) : 4); o->fmt = A(3); o->flags = A(2); OUT(5, b); }
static uint32_t new_surface(uint32_t w, uint32_t h, uint32_t fmt) { uint32_t sf = make(C_SURF); Obj *o = O(sf); o->w = w; o->h = h; o->fmt = fmt; o->levels = 1; return sf; }
static void dev_creatert(CPU *c, uint32_t s) { (void)s; OUT(7, new_surface(A(1), A(2), A(3))); }
static void dev_createoff(CPU *c, uint32_t s) { (void)s; OUT(5, new_surface(A(1), A(2), A(3))); }
static uint32_t backbuf, depthbuf;
static void dev_getbackbuffer(CPU *c, uint32_t s) { Obj *d = O(s); if (!backbuf) backbuf = new_surface(d->w, d->h, d->fmt); OUT(4, backbuf); }
static void dev_getrt(CPU *c, uint32_t s) { Obj *d = O(s); if (!backbuf) backbuf = new_surface(d->w, d->h, d->fmt); OUT(2, backbuf); }
static void dev_getds(CPU *c, uint32_t s) { Obj *d = O(s); if (!depthbuf) depthbuf = new_surface(d->w, d->h, 75); OUT(1, depthbuf); }
static void dev_gen2(CPU *c, uint32_t s) { (void)s; OUT(2, make(C_GEN)); }
static void dev_gen1(CPU *c, uint32_t s) { (void)s; OUT(1, make(C_SB)); }
static void dev_sb2(CPU *c, uint32_t s) { (void)s; OUT(2, make(C_SB)); }
static void dev_genvol(CPU *c, uint32_t s) { (void)s; OUT(8, make(C_GEN)); }
static void dev_createquery(CPU *c, uint32_t s) { (void)s; OUT(2, make(C_QUERY)); }
static void dev_getswap(CPU *c, uint32_t s) { (void)s; OUT(2, make(C_SWAP)); }
static void dev_null2(CPU *c, uint32_t s) { (void)s; OUT(2, 0); }
static void dev_null1(CPU *c, uint32_t s) { (void)s; OUT(1, 0); }
static void dev_nullstream(CPU *c, uint32_t s) { (void)s; OUT(2, 0); OUT(3, 0); OUT(4, 0); }
static void dev_getcaps(CPU *c, uint32_t s) { (void)s; fill_caps(A(1)); }
static void dev_getdispmode(CPU *c, uint32_t s) { Obj *d = O(s); put_mode(A(2), d->w, d->h); }
static void dev_getviewport(CPU *c, uint32_t s) { Obj *d = O(s); uint32_t p = A(1); memset(GP(p), 0, 24); WR32(p + 8, d->w); WR32(p + 12, d->h); WR32(p + 20, 0x3F800000u); }
static void dev_val3(CPU *c, uint32_t s) { (void)s; OUT(3, 0); }
static void dev_val2(CPU *c, uint32_t s) { (void)s; OUT(2, 0); }
static void present(void) {
    g_frames++;
    if (g_frames == 1) port_log("first Present: the game is rendering frames (null renderer)");
    if (g_max_frames && g_frames >= g_max_frames) { port_log("stopping after %u frames", g_frames); port_exit(0); }
    struct timespec t = { 0, 4000000L }; nanosleep(&t, NULL);     /* ~250 frames/s at most, so a headless run does not spin the CPU */
}
static void dev_present(CPU *c, uint32_t s) { (void)c; (void)s; present(); }
static void dev_testcoop(CPU *c, uint32_t s) { (void)s; c->eax = 0; }

/* textures and surfaces */
static void tex_leveldesc(CPU *c, uint32_t s) {
    Obj *o = O(s); uint32_t l = A(1), p = A(2); uint32_t w = o->w >> l, h = o->h >> l; memset(GP(p), 0, 32); WR32(p, o->fmt); WR32(p + 4, 1); WR32(p + 24, w ? w : 1); WR32(p + 28, h ? h : 1);
}
static void tex_getsurf(CPU *c, uint32_t s) {
    Obj *o = O(s); uint32_t l = A(1), pitch; uint32_t w = o->w >> l, h = o->h >> l; uint32_t sf = new_surface(w ? w : 1, h ? h : 1, o->fmt); Obj *so = O(sf); so->data = lvl_data(o, l, &pitch); OUT(2, sf);
}
static void tex_lock(CPU *c, uint32_t s) { Obj *o = O(s); uint32_t pitch, d = lvl_data(o, A(1), &pitch), p = A(2); WR32(p, pitch); WR32(p + 4, d); }
static void tex_levels(CPU *c, uint32_t s) { c->eax = O(s)->levels; }
static void surf_desc(CPU *c, uint32_t s) { Obj *o = O(s); uint32_t p = A(1); memset(GP(p), 0, 32); WR32(p, o->fmt); WR32(p + 4, 1); WR32(p + 24, o->w); WR32(p + 28, o->h); }
static void surf_lock(CPU *c, uint32_t s) {
    Obj *o = O(s); uint32_t pitch, sz; fmt_info(o->fmt, o->w, o->h, &pitch, &sz); if (!o->data) o->data = g_alloc(sz ? sz : 4); uint32_t p = A(1); WR32(p, pitch); WR32(p + 4, o->data);
}
static void buf_lock(CPU *c, uint32_t s) { Obj *o = O(s); OUT(3, o->data + A(1)); }
static void buf_desc(CPU *c, uint32_t s) { Obj *o = O(s); uint32_t p = A(1); memset(GP(p), 0, 24); WR32(p, o->fmt); WR32(p + 4, 10); WR32(p + 8, o->flags); WR32(p + 16, o->size); }
static void q_getdata(CPU *c, uint32_t s) { (void)s; if (A(1) && A(2) >= 4) WR32(A(1), 1); c->eax = 0; }
static void q_size(CPU *c, uint32_t s) { (void)s; c->eax = 4; }

/* ---------------------------------------------------------------- DirectSound */
static void ds_createbuf(CPU *c, uint32_t s) {
    (void)s; uint32_t d = A(1), b = make(C_DSB); Obj *o = O(b); o->flags = RD32(d + 4); o->size = RD32(d + 8); o->wfx = RD32(d + 16);
    if (o->size) o->data = g_alloc(o->size); if (o->wfx) o->avg = RD32(o->wfx + 8); if (!o->avg) o->avg = 176400; OUT(2, b);
}
static void ds_dup(CPU *c, uint32_t s) { (void)s; uint32_t src = A(1), b = make(C_DSB); Obj *o = O(b), *so = O(src); *o = *so; o->playing = 0; OUT(2, b); }
static void ds_getcaps(CPU *c, uint32_t s) { (void)s; uint32_t p = A(1), sz = RD32(p); memset(GP(p), 0, sz); WR32(p, sz); WR32(p + 4, 0x1F3); WR32(p + 8, 8000); WR32(p + 12, 192000); WR32(p + 16, 8); WR32(p + 20, 8); WR32(p + 24, 8); WR32(p + 28, 8); }
static void ds_speaker(CPU *c, uint32_t s) { (void)s; OUT(1, 4); }
static uint32_t dsb_pos(Obj *o) {
    if (!o->size) return 0; if (!o->playing) return o->loops_start_ms; uint64_t el = (uint64_t)(ms_now() - o->loops_start_ms) * o->avg / 1000; return (uint32_t)(el % o->size);
}
static void dsb_getpos(CPU *c, uint32_t s) { Obj *o = O(s); uint32_t p = dsb_pos(o); OUT(1, p); if (o->size) OUT(2, (p + o->avg / 20) % o->size); else OUT(2, 0); }
static void dsb_status(CPU *c, uint32_t s) {
    Obj *o = O(s); if (o->playing && !o->looping && o->size && (uint64_t)(ms_now() - o->loops_start_ms) * o->avg / 1000 >= o->size) o->playing = 0;
    OUT(1, o->playing ? (1u | (o->looping ? 4u : 0u)) : 0u);
}
static void dsb_play(CPU *c, uint32_t s) { (void)c; Obj *o = O(s); o->playing = 1; o->looping = A(3) & 1; o->loops_start_ms = ms_now(); }
static void dsb_stop(CPU *c, uint32_t s) { (void)c; O(s)->playing = 0; }
static void dsb_lock(CPU *c, uint32_t s) {
    Obj *o = O(s); uint32_t off = A(1), n = A(2), flags = A(7); if (flags & 2) { off = 0; n = o->size; } if (off > o->size) off = o->size;
    uint32_t c1 = n, c2 = 0; if (off + n > o->size) { c1 = o->size - off; c2 = n - c1; if (c2 > off) c2 = off; }
    OUT(3, o->data + off); OUT(4, c1); OUT(5, c2 ? o->data : 0); OUT(6, c2);
}
static void dsb_caps(CPU *c, uint32_t s) { Obj *o = O(s); uint32_t p = A(1); WR32(p + 4, o->flags); WR32(p + 8, o->size); }
static uint32_t dsb_fmtsize(Obj *o) { return o->wfx ? 18 + (RD32(o->wfx + 16) & 0xffff) : 18; }
static void dsb_getformat(CPU *c, uint32_t s) {
    Obj *o = O(s);
    if (!o->wfx) { o->wfx = g_alloc(32); memset(GP(o->wfx), 0, 32); WR32(o->wfx, 1 | (2 << 16)); WR32(o->wfx + 4, 44100); WR32(o->wfx + 8, 44100 * 4); WR32(o->wfx + 12, 4 | (16 << 16)); }
    uint32_t n = dsb_fmtsize(o), p = A(1), cb = A(2);
    if (p && cb) memcpy(GP(p), GP(o->wfx), cb < n ? cb : n);
    OUT(3, n);
}
static void dsb_setformat(CPU *c, uint32_t s) {
    Obj *o = O(s); uint32_t p = A(1);
    if (p) { uint32_t n = 18 + (RD32(p + 16) & 0xffff); uint32_t b = g_alloc(n + 4); memcpy(GP(b), GP(p), n); o->wfx = b; o->avg = RD32(p + 8) ? RD32(p + 8) : o->avg; }
}
static void dsb_zero1(CPU *c, uint32_t s) { (void)s; OUT(1, 0); }

/* ---------------------------------------------------------------- DirectInput */
static void di_createdev(CPU *c, uint32_t s) { (void)s; OUT(2, make(C_DID)); }
static void dd_caps(CPU *c, uint32_t s) { (void)s; for (int i = 1; i <= 2; i++) { uint32_t p = A(i); if (!p) continue; uint32_t sz = RD32(p); memset(GP(p + 4), 0xFF, sz - 4); WR32(p + 60, 0x10000000u); WR32(p + 64, 0x10000000u); } }
static void did_state(CPU *c, uint32_t s) { (void)s; memset(GP(A(2)), 0, A(1)); }
static void did_data(CPU *c, uint32_t s) { (void)s; OUT(3, 0); }
static void did_caps(CPU *c, uint32_t s) { (void)s; uint32_t p = A(1), sz = RD32(p); memset(GP(p + 4), 0, sz - 4); }

static const Handler hlist[] = {
    { C_D3D9, "GetAdapterIdentifier", d3d_adapter_id }, { C_D3D9, "GetAdapterModeCount", d3d_modecount }, { C_D3D9, "EnumAdapterModes", d3d_enummodes },
    { C_D3D9, "GetAdapterDisplayMode", d3d_dispmode }, { C_D3D9, "GetAdapterCount", d3d_adaptercount }, { C_D3D9, "CheckDeviceMultiSampleType", d3d_msaa },
    { C_D3D9, "GetDeviceCaps", d3d_getcaps }, { C_D3D9, "GetAdapterMonitor", d3d_monitor }, { C_D3D9, "CreateDevice", d3d_createdevice },
    { C_DEV, "CreateTexture", dev_createtexture }, { C_DEV, "CreateCubeTexture", dev_createcube }, { C_DEV, "CreateVertexBuffer", dev_createvb }, { C_DEV, "CreateIndexBuffer", dev_createib },
    { C_DEV, "CreateRenderTarget", dev_creatert }, { C_DEV, "CreateDepthStencilSurface", dev_creatert }, { C_DEV, "CreateOffscreenPlainSurface", dev_createoff },
    { C_DEV, "GetBackBuffer", dev_getbackbuffer }, { C_DEV, "GetRenderTarget", dev_getrt }, { C_DEV, "GetDepthStencilSurface", dev_getds },
    { C_DEV, "CreateVertexDeclaration", dev_gen2 }, { C_DEV, "CreateVertexShader", dev_gen2 }, { C_DEV, "CreatePixelShader", dev_gen2 }, { C_DEV, "CreateStateBlock", dev_sb2 },
    { C_DEV, "EndStateBlock", dev_gen1 }, { C_DEV, "CreateVolumeTexture", dev_genvol }, { C_DEV, "CreateQuery", dev_createquery }, { C_DEV, "GetSwapChain", dev_getswap },
    { C_DEV, "GetTexture", dev_null2 }, { C_DEV, "GetStreamSource", dev_nullstream }, { C_DEV, "GetIndices", dev_null1 }, { C_DEV, "GetVertexDeclaration", dev_null1 },
    { C_DEV, "GetVertexShader", dev_null1 }, { C_DEV, "GetPixelShader", dev_null1 }, { C_DEV, "GetDeviceCaps", dev_getcaps }, { C_DEV, "GetDisplayMode", dev_getdispmode },
    { C_DEV, "GetViewport", dev_getviewport }, { C_DEV, "GetRenderState", dev_val2 }, { C_DEV, "GetTextureStageState", dev_val3 }, { C_DEV, "GetSamplerState", dev_val3 },
    { C_DEV, "Present", dev_present }, { C_DEV, "TestCooperativeLevel", dev_testcoop },
    { C_TEX, "GetLevelDesc", tex_leveldesc }, { C_TEX, "GetSurfaceLevel", tex_getsurf }, { C_TEX, "LockRect", tex_lock }, { C_TEX, "GetLevelCount", tex_levels },
    { C_SURF, "GetDesc", surf_desc }, { C_SURF, "LockRect", surf_lock }, { C_BUF, "Lock", buf_lock }, { C_BUF, "GetDesc", buf_desc },
    { C_QUERY, "GetData", q_getdata }, { C_QUERY, "GetDataSize", q_size }, { C_SWAP, "Present", dev_present },
    { C_DS, "CreateSoundBuffer", ds_createbuf }, { C_DS, "DuplicateSoundBuffer", ds_dup }, { C_DS, "GetCaps", ds_getcaps }, { C_DS, "GetSpeakerConfig", ds_speaker },
    { C_DSB, "GetCurrentPosition", dsb_getpos }, { C_DSB, "GetStatus", dsb_status }, { C_DSB, "Play", dsb_play }, { C_DSB, "Stop", dsb_stop }, { C_DSB, "Lock", dsb_lock },
    { C_DSB, "GetCaps", dsb_caps }, { C_DSB, "GetFormat", dsb_getformat }, { C_DSB, "SetFormat", dsb_setformat }, { C_DSB, "GetVolume", dsb_zero1 }, { C_DSB, "GetPan", dsb_zero1 }, { C_DSB, "GetFrequency", dsb_zero1 },
    { C_KSP, "QuerySupport", ksp_query }, { C_KSP, "Get", ksp_fail }, { C_KSP, "Set", ksp_fail },
    { C_DD, "GetCaps", dd_caps }, { C_DI, "CreateDevice", di_createdev }, { C_DID, "GetDeviceState", did_state }, { C_DID, "GetDeviceData", did_data }, { C_DID, "GetCapabilities", did_caps },
};
static void install_unknown(void) {
    static int done; if (done) return; done = 1;
    static Handler all[sizeof hlist / sizeof *hlist + 3 * C_NCLASS]; int n = 0;
    for (unsigned i = 0; i < sizeof hlist / sizeof *hlist; i++) all[n++] = hlist[i];
    for (int cl = 0; cl < C_NCLASS; cl++) { all[n++] = (Handler){ cl, "QueryInterface", h_qi }; all[n++] = (Handler){ cl, "AddRef", h_addref }; all[n++] = (Handler){ cl, "Release", h_release }; }
    handlers_tab = all; nhandlers = n;
}

/* ---------------------------------------------------------------- the DLL exports */
SHIM(Direct3DCreate9) { install_unknown(); RET(make(C_D3D9)); }
SHIM(DirectSoundCreate) { install_unknown(); uint32_t o = make(C_DS); WR32(A(1), o); RET(0); }
SHIM(DirectInputCreateA) { install_unknown(); uint32_t o = make(C_DI); WR32(A(2), o); RET(0); }
SHIM(DirectDrawCreate) { install_unknown(); uint32_t o = make(C_DD); WR32(A(1), o); RET(0); }
SHIM(DirectDrawEnumerateA) { g_call(c, A(0), 3 + 1, 0u, g_str("Primary Display Driver"), g_str("display"), A(1)); RET(0); }
SHIM(D3DXFail) { RET(0x8876086Cu); }   /* D3DERR_INVALIDCALL: no effect files; the game falls back to the fixed-function path */
SHIM(Ret0x) { RET(0); }
const ShimDef com_shims[] = {
    { "Direct3DCreate9", sh_Direct3DCreate9, STD(1) }, { "DirectSoundCreate", sh_DirectSoundCreate, STD(3) }, { "DirectInputCreateA", sh_DirectInputCreateA, STD(4) }, { "DirectDrawCreate", sh_DirectDrawCreate, STD(3) }, { "DirectDrawEnumerateA", sh_DirectDrawEnumerateA, STD(2) },
    { "D3DXCreateEffect", sh_D3DXFail, STD(9) }, { "D3DXFilterTexture", sh_Ret0x, STD(4) }, { "D3DXLoadSurfaceFromSurface", sh_Ret0x, STD(8) },
    { "GetFileVersionInfoSizeA", sh_Ret0x, STD(2) }, { "GetFileVersionInfoA", sh_Ret0x, STD(4) }, { "VerQueryValueA", sh_Ret0x, STD(4) },
    { 0, 0, 0 }
};

/* slot number of a method in one of the interfaces above (for tests and tools) */
int com_slot(const char *iface, const char *method) {
    for (int cl = 0; cl < C_NCLASS; cl++) {
        if (strcmp(ifaces[cl].name, iface)) continue;
        char spec[8192]; snprintf(spec, sizeof spec, "%s", ifaces[cl].spec); int n = 0; char *save;
        for (char *t = strtok_r(spec, " ", &save); t; t = strtok_r(NULL, " ", &save), n++) { char *col = strchr(t, ':'); *col = 0; if (!strcmp(t, method)) return n; }
    }
    return -1;
}
