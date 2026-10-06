/* api_usage.inc: included by win_host.c. With darkrecomp_apistats.txt next to the exe, records every call the game makes into the
 * four multimedia APIs it loads at run time (d3d9, d3dx9, dsound, dinput): interface, method, how often, and for the state-setting
 * calls the distinct values. The report (darkrecomp_api_usage.txt, rewritten about every 5 seconds of play) is what a porter has to
 * provide: it lists exactly which COM methods, render states, texture formats and so on the engine uses.
 * Objects are named by where they came from (CreateTexture -> IDirect3DTexture9 ...), so the same vtable always gets the same name. */

#define AU_MAX_CALLS 2048
#define AU_MAX_VALS 8192
typedef struct { char iface[40]; int slot; unsigned n; } AuCall;
typedef struct { char what[48]; unsigned a, b, c; unsigned n; } AuVal;
static AuCall au_calls[AU_MAX_CALLS]; static int au_ncalls;
static AuVal au_vals[AU_MAX_VALS]; static int au_nvals;
typedef struct { void *vt; char name[40]; } AuTag;
static AuTag au_tags[256]; static int au_ntags;
static int g_apistats; static unsigned au_presents;

static const char *const au_dev[] = { "QueryInterface","AddRef","Release","TestCooperativeLevel","GetAvailableTextureMem","EvictManagedResources","GetDirect3D","GetDeviceCaps","GetDisplayMode","GetCreationParameters","SetCursorProperties","SetCursorPosition","ShowCursor","CreateAdditionalSwapChain","GetSwapChain","GetNumberOfSwapChains","Reset","Present","GetBackBuffer","GetRasterStatus","SetDialogBoxMode","SetGammaRamp","GetGammaRamp","CreateTexture","CreateVolumeTexture","CreateCubeTexture","CreateVertexBuffer","CreateIndexBuffer","CreateRenderTarget","CreateDepthStencilSurface","UpdateSurface","UpdateTexture","GetRenderTargetData","GetFrontBufferData","StretchRect","ColorFill","CreateOffscreenPlainSurface","SetRenderTarget","GetRenderTarget","SetDepthStencilSurface","GetDepthStencilSurface","BeginScene","EndScene","Clear","SetTransform","GetTransform","MultiplyTransform","SetViewport","GetViewport","SetMaterial","GetMaterial","SetLight","GetLight","LightEnable","GetLightEnable","SetClipPlane","GetClipPlane","SetRenderState","GetRenderState","CreateStateBlock","BeginStateBlock","EndStateBlock","SetClipStatus","GetClipStatus","GetTexture","SetTexture","GetTextureStageState","SetTextureStageState","GetSamplerState","SetSamplerState","ValidateDevice","SetPaletteEntries","GetPaletteEntries","SetCurrentTexturePalette","GetCurrentTexturePalette","SetScissorRect","GetScissorRect","SetSoftwareVertexProcessing","GetSoftwareVertexProcessing","SetNPatchMode","GetNPatchMode","DrawPrimitive","DrawIndexedPrimitive","DrawPrimitiveUP","DrawIndexedPrimitiveUP","ProcessVertices","CreateVertexDeclaration","SetVertexDeclaration","GetVertexDeclaration","SetFVF","GetFVF","CreateVertexShader","SetVertexShader","GetVertexShader","SetVertexShaderConstantF","GetVertexShaderConstantF","SetVertexShaderConstantI","GetVertexShaderConstantI","SetVertexShaderConstantB","GetVertexShaderConstantB","SetStreamSource","GetStreamSource","SetStreamSourceFreq","GetStreamSourceFreq","SetIndices","GetIndices","CreatePixelShader","SetPixelShader","GetPixelShader","SetPixelShaderConstantF","GetPixelShaderConstantF","SetPixelShaderConstantI","GetPixelShaderConstantI","SetPixelShaderConstantB","GetPixelShaderConstantB","DrawRectPatch","DrawTriPatch","DeletePatch","CreateQuery" };
static const char *const au_d3d9[] = { "QueryInterface","AddRef","Release","RegisterSoftwareDevice","GetAdapterCount","GetAdapterIdentifier","GetAdapterModeCount","EnumAdapterModes","GetAdapterDisplayMode","CheckDeviceType","CheckDeviceFormat","CheckDeviceMultiSampleType","CheckDepthStencilMatch","CheckDeviceFormatConversion","GetDeviceCaps","GetAdapterMonitor","CreateDevice" };
#define AU_RES "GetDevice","SetPrivateData","GetPrivateData","FreePrivateData","SetPriority","GetPriority","PreLoad","GetType"
static const char *const au_tex[] = { "QueryInterface","AddRef","Release", AU_RES, "SetLOD","GetLOD","GetLevelCount","SetAutoGenFilterType","GetAutoGenFilterType","GenerateMipSubLevels","GetLevelDesc","GetSurfaceLevel","LockRect","UnlockRect","AddDirtyRect" };
static const char *const au_cube[] = { "QueryInterface","AddRef","Release", AU_RES, "SetLOD","GetLOD","GetLevelCount","SetAutoGenFilterType","GetAutoGenFilterType","GenerateMipSubLevels","GetLevelDesc","GetCubeMapSurface","LockRect","UnlockRect","AddDirtyRect" };
static const char *const au_surf[] = { "QueryInterface","AddRef","Release", AU_RES, "GetContainer","GetDesc","LockRect","UnlockRect","GetDC","ReleaseDC" };
static const char *const au_buf[] = { "QueryInterface","AddRef","Release", AU_RES, "Lock","Unlock","GetDesc" };
static const char *const au_swap[] = { "QueryInterface","AddRef","Release", "Present","GetFrontBufferData","GetBackBuffer","GetRasterStatus","GetDisplayMode","GetDevice","GetPresentParameters" };
static const char *const au_ds[] = { "QueryInterface","AddRef","Release","CreateSoundBuffer","GetCaps","DuplicateSoundBuffer","SetCooperativeLevel","Compact","GetSpeakerConfig","SetSpeakerConfig","Initialize" };
static const char *const au_dsb[] = { "QueryInterface","AddRef","Release","GetCaps","GetCurrentPosition","GetFormat","GetVolume","GetPan","GetFrequency","GetStatus","Initialize","Lock","Play","SetCurrentPosition","SetFormat","SetVolume","SetPan","SetFrequency","Stop","Unlock","Restore" };
static const char *const au_ds3b[] = { "QueryInterface","AddRef","Release","GetAllParameters","GetConeAngles","GetConeOrientation","GetConeOutsideVolume","GetMaxDistance","GetMinDistance","GetMode","GetPosition","GetVelocity","SetAllParameters","SetConeAngles","SetConeOrientation","SetConeOutsideVolume","SetMaxDistance","SetMinDistance","SetMode","SetPosition","SetVelocity" };
static const char *const au_ds3l[] = { "QueryInterface","AddRef","Release","GetAllParameters","GetDistanceFactor","GetDopplerFactor","GetOrientation","GetPosition","GetRolloffFactor","SetAllParameters","SetDistanceFactor","SetDopplerFactor","SetOrientation","SetPosition","SetRolloffFactor","CommitDeferredSettings" };
static const char *const au_di[] = { "QueryInterface","AddRef","Release","CreateDevice","EnumDevices","GetDeviceStatus","RunControlPanel","Initialize" };
static const char *const au_did[] = { "QueryInterface","AddRef","Release","GetCapabilities","EnumObjects","GetProperty","SetProperty","Acquire","Unacquire","GetDeviceState","GetDeviceData","SetDataFormat","SetEventNotification","SetCooperativeLevel","GetObjectInfo","GetDeviceInfo","RunControlPanel","Initialize" };
typedef struct { const char *iface; const char *const *names; int n; } AuIf;
#define AUIF(s, t) { s, t, (int)(sizeof t / sizeof *t) }
static const AuIf au_ifs[] = { AUIF("IDirect3DDevice9", au_dev), AUIF("IDirect3D9", au_d3d9), AUIF("IDirect3DTexture9", au_tex), AUIF("IDirect3DCubeTexture9", au_cube),
    AUIF("IDirect3DSurface9", au_surf), AUIF("IDirect3DVertexBuffer9", au_buf), AUIF("IDirect3DIndexBuffer9", au_buf), AUIF("IDirect3DSwapChain9", au_swap),
    AUIF("IDirectSound", au_ds), AUIF("IDirectSoundBuffer", au_dsb), AUIF("IDirectSound3DBuffer", au_ds3b), AUIF("IDirectSound3DListener", au_ds3l),
    AUIF("IDirectInputA", au_di), AUIF("IDirectInputDeviceA", au_did) };

static const char *au_method(const char *iface, int slot, char *buf, size_t n) {
    for (unsigned i = 0; i < sizeof au_ifs / sizeof *au_ifs; i++)
        if (!strcmp(au_ifs[i].iface, iface) && slot >= 0 && slot < au_ifs[i].n) return au_ifs[i].names[slot];
    snprintf(buf, n, "slot%d", slot); return buf;
}
static void au_tag(uint32_t obj, const char *name) {
    if (obj < 0x10000 || IsBadReadPtr((void *)(uintptr_t)obj, 4)) return;
    void *vt = *(void **)(uintptr_t)obj;
    for (int i = 0; i < au_ntags; i++) if (au_tags[i].vt == vt) return;
    if (au_ntags < 256) { au_tags[au_ntags].vt = vt; snprintf(au_tags[au_ntags].name, sizeof au_tags[0].name, "%s", name); au_ntags++; }
}
static void au_tag_out(uint32_t pp, const char *name) { if (pp > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)pp, 4)) au_tag(*(uint32_t *)(uintptr_t)pp, name); }
static void au_call(const char *iface, int slot) {
    for (int i = 0; i < au_ncalls; i++) if (au_calls[i].slot == slot && !strcmp(au_calls[i].iface, iface)) { au_calls[i].n++; return; }
    if (au_ncalls < AU_MAX_CALLS) { snprintf(au_calls[au_ncalls].iface, sizeof au_calls[0].iface, "%s", iface); au_calls[au_ncalls].slot = slot; au_calls[au_ncalls].n = 1; au_ncalls++; }
}
static void au_val(const char *what, unsigned a, unsigned b, unsigned c) {
    for (int i = 0; i < au_nvals; i++) if (au_vals[i].a == a && au_vals[i].b == b && au_vals[i].c == c && !strcmp(au_vals[i].what, what)) { au_vals[i].n++; return; }
    if (au_nvals < AU_MAX_VALS) { snprintf(au_vals[au_nvals].what, sizeof au_vals[0].what, "%s", what); au_vals[au_nvals].a = a; au_vals[au_nvals].b = b; au_vals[au_nvals].c = c; au_vals[au_nvals].n = 1; au_nvals++; }
}
static int au_cmp_call(const void *x, const void *y) { const AuCall *p = x, *q = y; int r = strcmp(p->iface, q->iface); return r ? r : p->slot - q->slot; }
static int au_cmp_val(const void *x, const void *y) { const AuVal *p = x, *q = y; int r = strcmp(p->what, q->what); if (r) return r; return p->a != q->a ? (p->a < q->a ? -1 : 1) : p->b != q->b ? (p->b < q->b ? -1 : 1) : (p->c < q->c ? -1 : p->c > q->c); }
static void au_write(void) {
    char f[MAX_PATH]; snprintf(f, sizeof f, "%s\\darkrecomp_api_usage.txt", exe_dir);
    FILE *o = fopen(f, "w"); if (!o) return;
    fprintf(o, "# Calls the game made into d3d9 / d3dx9 / dsound / dinput (until %u Presents). Each line: interface::method  count\n", au_presents);
    qsort(au_calls, au_ncalls, sizeof *au_calls, au_cmp_call);
    for (int i = 0; i < au_ncalls; i++) { char b[24]; fprintf(o, "call %s::%s %u\n", au_calls[i].iface, au_method(au_calls[i].iface, au_calls[i].slot, b, sizeof b), au_calls[i].n); }
    qsort(au_vals, au_nvals, sizeof *au_vals, au_cmp_val);
    fprintf(o, "# Distinct argument values (hex). SetRenderState: state value; TextureStageState: stage type value; SamplerState: sampler type value;\n"
               "# Create*: formats are D3DFORMAT, pools D3DPOOL (0 default, 1 managed, 2 systemmem), prim = D3DPRIMITIVETYPE\n");
    for (int i = 0; i < au_nvals; i++) fprintf(o, "val %s %x %x %x  x%u\n", au_vals[i].what, au_vals[i].a, au_vals[i].b, au_vals[i].c, au_vals[i].n);
    fclose(o);
}
/* before a native call: if it is a method of a COM object in one of the four APIs (fn found in the vtable of args[0]), count it */
static int au_cur_slot; static char au_cur_iface[40];
static void au_pre(void *fn, const uint32_t *a) {
    au_cur_slot = -1;
    HMODULE m = NULL; char mn[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m) || !m) return;
    GetModuleFileNameA(m, mn, sizeof mn); const char *b = strrchr(mn, '\\'); b = b ? b + 1 : mn;
    int which = !_stricmp(b, "d3d9.dll") ? 1 : !_strnicmp(b, "d3dx9_", 6) ? 2 : !_stricmp(b, "dsound.dll") ? 3 : !_stricmp(b, "dinput.dll") ? 4 : 0;
    if (!which) return;
    if (a[0] < 0x10000 || IsBadReadPtr((void *)(uintptr_t)a[0], 4)) return;
    void **vt = *(void ***)(uintptr_t)a[0]; if (!vt || IsBadReadPtr(vt, 64 * sizeof *vt)) return;
    int k; for (k = 0; k < 160; k++) { if (k && !(k & 63) && IsBadReadPtr(vt + k, 64 * sizeof *vt)) break; if (vt[k] == fn) break; }
    if (k >= 160 || vt[k] != fn) return;
    const char *nm = NULL; char fb[40];
    if (g_dev && vt == *(void ***)g_dev) nm = "IDirect3DDevice9";
    for (int i = 0; !nm && i < au_ntags; i++) if (au_tags[i].vt == (void *)vt) nm = au_tags[i].name;
    if (!nm) { snprintf(fb, sizeof fb, "unknown-%s-vt%x", which == 1 ? "d3d9" : which == 2 ? "d3dx9" : which == 3 ? "dsound" : "dinput", (unsigned)((uintptr_t)vt - (uintptr_t)m)); nm = fb; }
    au_cur_slot = k; snprintf(au_cur_iface, sizeof au_cur_iface, "%s", nm);
    au_call(nm, k);
    if (!strcmp(nm, "IDirect3DDevice9")) {
        switch (k) {
        case 57: au_val("SetRenderState", a[1], a[2], 0); break;
        case 67: au_val("SetTextureStageState", a[1], a[2], a[3]); break;
        case 69: au_val("SetSamplerState", a[1], a[2], a[3]); break;
        case 44: au_val("SetTransform", a[1], 0, 0); break;
        case 89: au_val("SetFVF", a[1], 0, 0); break;
        case 81: case 82: au_val("DrawPrim", k, a[1], 0); break;
        case 83: au_val("DrawPrimitiveUP", a[1], a[4], 0); break;                /* primitive type, vertex stride */
        case 84: au_val("DrawIndexedPrimitiveUP", a[1], a[6], a[8]); break;      /* primitive type, index format, stride */
        case 23: au_val("CreateTexture", a[5], a[6], a[4]); break;          /* format, pool, usage */
        case 25: au_val("CreateCubeTexture", a[4], a[5], a[3]); break;
        case 26: au_val("CreateVertexBuffer", a[3], a[4], a[2]); break;     /* fvf, pool, usage */
        case 27: au_val("CreateIndexBuffer", a[3], a[4], a[2]); break;      /* format, pool, usage */
        case 28: au_val("CreateRenderTarget", a[3], a[4], 0); break;        /* format, multisample */
        case 29: au_val("CreateDepthStencilSurface", a[3], a[4], 0); break;
        case 36: au_val("CreateOffscreenPlainSurface", a[3], a[4], 0); break;
        case 34: au_val("StretchRect", a[5], 0, 0); break;                  /* filter */
        case 43: au_val("Clear", a[3], 0, 0); break;                        /* flags */
        case 100: au_val("SetStreamSource", a[1], a[4], 0); break;            /* stream, stride */
        case 17: if (!(++au_presents % 300)) au_write(); break;
        }
    }
    if (!strcmp(nm, "IDirectSound")) { if (k == 3 && a[1] > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)a[1], 8)) au_val("CreateSoundBuffer-flags", *(unsigned *)(uintptr_t)(a[1] + 4), 0, 0); }
}
/* after the call: objects handed back through out-parameters get named after the call that made them */
static void au_post(const uint32_t *a, uint32_t hr) {
    if (au_cur_slot < 0 || hr) return;
    const char *nm = au_cur_iface; int k = au_cur_slot;
    if (!strcmp(nm, "IDirect3DDevice9")) {
        switch (k) {
        case 6: au_tag_out(a[1], "IDirect3D9"); break;
        case 14: au_tag_out(a[2], "IDirect3DSwapChain9"); break;
        case 13: au_tag_out(a[2], "IDirect3DSwapChain9"); break;
        case 18: au_tag_out(a[4], "IDirect3DSurface9"); break;
        case 23: au_tag_out(a[7], "IDirect3DTexture9"); break;
        case 25: au_tag_out(a[6], "IDirect3DCubeTexture9"); break;
        case 26: au_tag_out(a[5], "IDirect3DVertexBuffer9"); break;
        case 27: au_tag_out(a[5], "IDirect3DIndexBuffer9"); break;
        case 28: case 29: au_tag_out(a[7], "IDirect3DSurface9"); break;
        case 36: au_tag_out(a[5], "IDirect3DSurface9"); break;
        case 38: au_tag_out(a[2], "IDirect3DSurface9"); break;
        case 40: au_tag_out(a[1], "IDirect3DSurface9"); break;
        }
    } else if (!strcmp(nm, "IDirect3DTexture9") && k == 18) au_tag_out(a[2], "IDirect3DSurface9");
    else if (!strcmp(nm, "IDirect3DCubeTexture9") && k == 18) au_tag_out(a[3], "IDirect3DSurface9");
    else if (!strcmp(nm, "IDirectSound") && k == 3) au_tag_out(a[2], "IDirectSoundBuffer");
    else if (!strcmp(nm, "IDirectInputA") && k == 3) au_tag_out(a[2], "IDirectInputDeviceA");
    else if (!strcmp(nm, "IDirectSoundBuffer") && k == 0 && a[1] > 0x10000 && !IsBadReadPtr((void *)(uintptr_t)a[1], 4)) {   /* QueryInterface: which IID? */
        unsigned d = *(unsigned *)(uintptr_t)a[1];
        au_val("QueryInterface-iid", d, 0, 0);
        if (d == 0x279AFA86u) au_tag_out(a[2], "IDirectSound3DBuffer"); else if (d == 0x279AFA84u) au_tag_out(a[2], "IDirectSound3DListener");
    }
}
/* the functions the engine fetches with GetProcAddress and calls directly (factory entry points) */
static void au_entry(void *fn, const uint32_t *a, uint32_t ret) {
    HMODULE m = NULL; char mn[MAX_PATH];
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)fn, &m) || !m) return;
    GetModuleFileNameA(m, mn, sizeof mn); const char *b = strrchr(mn, '\\'); b = b ? b + 1 : mn;
    FARPROC p;
    if (!_stricmp(b, "d3d9.dll") && (p = GetProcAddress(m, "Direct3DCreate9")) && (void *)p == fn) { au_val("Direct3DCreate9", a[0], 0, 0); au_tag(ret, "IDirect3D9"); }
    else if (!_stricmp(b, "dsound.dll") && (p = GetProcAddress(m, "DirectSoundCreate")) && (void *)p == fn) { au_val("DirectSoundCreate", 0, 0, 0); au_tag_out(a[1], "IDirectSound"); }
    else if (!_stricmp(b, "dinput.dll") && (p = GetProcAddress(m, "DirectInputCreateA")) && (void *)p == fn) { au_val("DirectInputCreateA", a[1], 0, 0); au_tag_out(a[2], "IDirectInputA"); }
    else if (!_strnicmp(b, "d3dx9_", 6)) {
        if ((p = GetProcAddress(m, "D3DXCreateEffect")) && (void *)p == fn) { au_val("D3DXCreateEffect", a[5], 0, 0); au_tag_out(a[7], "ID3DXEffect"); }   /* flags */
        else if ((p = GetProcAddress(m, "D3DXFilterTexture")) && (void *)p == fn) au_val("D3DXFilterTexture", a[2], a[3], 0);
        else if ((p = GetProcAddress(m, "D3DXLoadSurfaceFromSurface")) && (void *)p == fn) au_val("D3DXLoadSurfaceFromSurface", a[5], 0, 0);
    }
}
