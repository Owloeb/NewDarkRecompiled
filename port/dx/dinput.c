/* dinput.c: DirectInput (version 7 interfaces) for the system keyboard and mouse, fed by the platform's input events
 * (the same ones user32.c turns into window messages). Device state honours the data format the game sets, and
 * buffered data (GetDeviceData) is queued while a device is acquired, as on Windows. An exclusively acquired mouse
 * switches the platform to relative mouse mode for mouse look. Joysticks are not offered. */
#include <stdio.h>
#include <stdlib.h>
#include "com.h"

#define DI_OK 0u
#define DI_NOEFFECT 1u
#define DI_BUFFEROVERFLOW 1u
#define DIERR_NOTACQUIRED 0x8007000Cu
#define DIERR_INPUTLOST 0x8007001Eu
#define DIERR_INVALIDPARAM 0x80070057u
#define DIERR_DEVICENOTREG 0x80040154u
#define DIERR_UNSUPPORTED 0x80004001u
extern ComClass c_di, c_did;
static const uint8_t g_kbd[16] = GUID_SysKeyboard, g_mouse[16] = GUID_SysMouse, g_x[16] = GUID_XAxis, g_y[16] = GUID_YAxis, g_z[16] = GUID_ZAxis;

typedef struct { uint32_t ofs, data, time, seq; } DiEvent;
typedef struct { int kind; int index; uint32_t ofs; } FmtObj;        /* kind: 0 key/button, 1 axis */
typedef struct Dev {
    struct Dev *next; int mouse; int acquired, exclusive, foreground;
    FmtObj *fmt; int nfmt; uint32_t fmt_size;
    DiEvent *q; uint32_t qcap, qn, qhead; int overflow; uint32_t seq;
    int32_t dx, dy, dz;          /* mouse motion since the last GetDeviceState */
    KObj *event; int absolute;
} Dev;
static Dev *devs;
static Dev *D(ComObj *o) { return (Dev *)o->data; }
static void mouse_mode(void) { int acq = 0, ex = 0; for (Dev *d = devs; d; d = d->next) if (d->mouse && d->acquired) { acq = 1; if (d->exclusive) ex = 1; } input_set_di_mouse(acq, ex); }

/* ---------------------------------------------------------------- events from the platform */
static void push(Dev *d, uint32_t ofs, uint32_t data) {
    if (!d->qcap) return;
    if (d->qn == d->qcap) { d->overflow = 1; d->qhead = (d->qhead + 1) % d->qcap; d->qn--; }
    d->q[(d->qhead + d->qn++) % d->qcap] = (DiEvent){ ofs, data, ms_ticks(), ++d->seq };
    if (d->event) kevent_set(d->event);
}
static int ofs_of(Dev *d, int kind, int index, uint32_t *ofs) { for (int i = 0; i < d->nfmt; i++) if (d->fmt[i].kind == kind && d->fmt[i].index == index) { *ofs = d->fmt[i].ofs; return 1; } return 0; }
static void on_input(const PlatEvent *e) {
    for (Dev *d = devs; d; d = d->next) {
        if (e->type == PLAT_EV_FOCUS && !e->down && d->foreground && d->acquired) { d->acquired = 0; mouse_mode(); }
        if (!d->acquired) continue;
        uint32_t o;
        if (!d->mouse && e->type == PLAT_EV_KEY && !e->repeat && ofs_of(d, 0, e->key & 0xFF, &o)) push(d, o, e->down ? 0x80 : 0);
        if (d->mouse) {
            if (e->type == PLAT_EV_MOUSE_MOVE) { d->dx += e->dx; d->dy += e->dy; if (e->dx && ofs_of(d, 1, 0, &o)) push(d, o, (uint32_t)e->dx); if (e->dy && ofs_of(d, 1, 1, &o)) push(d, o, (uint32_t)e->dy); }
            if (e->type == PLAT_EV_MOUSE_WHEEL) { d->dz += e->dy; if (ofs_of(d, 1, 2, &o)) push(d, o, (uint32_t)e->dy); }
            if (e->type == PLAT_EV_MOUSE_BUTTON && ofs_of(d, 0, e->button, &o)) push(d, o, e->down ? 0x80 : 0);
        }
    }
}

/* ---------------------------------------------------------------- IDirectInputDevice7A */
static void put_instance(uint32_t p, int mouse) {        /* DIDEVICEINSTANCEA (580 bytes) */
    uint32_t sz = RD32(p); if (sz < 20) return; memset(GP(p) + 4, 0, (sz > 580 ? 580 : sz) - 4);
    memcpy(GP(p + 4), mouse ? g_mouse : g_kbd, 16); memcpy(GP(p + 20), mouse ? g_mouse : g_kbd, 16);
    WR32(p + 36, mouse ? 0x0212u : 0x0403u);
    if (sz >= 556) { snprintf((char *)GP(p + 40), 260, "%s", mouse ? "Mouse" : "Keyboard"); snprintf((char *)GP(p + 300), 260, "%s", mouse ? "Mouse" : "Keyboard"); }
}
static void did_caps(CPU *c, ComObj *s) {
    Dev *d = D(s); uint32_t p = A(1), sz = RD32(p); if (sz < 24) { RET(DIERR_INVALIDPARAM); return; } memset(GP(p) + 4, 0, sz - 4);
    WR32(p + 4, 1); WR32(p + 8, d->mouse ? 0x0212u : 0x0403u); WR32(p + 12, d->mouse ? 3 : 0); WR32(p + 16, d->mouse ? 5 : 128); RET(0);
}
static void did_setformat(CPU *c, ComObj *s) {
    Dev *d = D(s); uint32_t f = A(1); if (!f) { RET(DIERR_INVALIDPARAM); return; } if (d->acquired) { RET(0x80070005u); return; }
    uint32_t nobj = RD32(f + 16), objs = RD32(f + 20), osz = RD32(f + 4); d->fmt_size = RD32(f + 12); d->absolute = (RD32(f + 8) & 1) != 0;
    free(d->fmt); d->fmt = calloc(nobj ? nobj : 1, sizeof *d->fmt); d->nfmt = 0; int next_axis = 0, next_button = 0;
    for (uint32_t i = 0; i < nobj; i++) {
        uint32_t o = objs + i * (osz ? osz : 16), guid = RD32(o), ofs = RD32(o + 4), type = RD32(o + 8);
        int inst = (int)((type >> 8) & 0xFFFF), any = inst == 0xFFFF;
        FmtObj *fo = &d->fmt[d->nfmt]; fo->ofs = ofs;
        if (type & 3) {                                    /* DIDFT_AXIS (REL 1 / ABS 2) */
            fo->kind = 1; fo->index = guid && com_iid_eq(guid, g_x) ? 0 : guid && com_iid_eq(guid, g_y) ? 1 : guid && com_iid_eq(guid, g_z) ? 2 : any ? next_axis : inst;
            next_axis = fo->index + 1; d->nfmt++;
        } else if (type & 0xC) {                           /* DIDFT_BUTTON (PSH 4 / TGL 8) */
            fo->kind = 0; fo->index = d->mouse ? (any ? next_button : inst) : (any ? next_button : inst); next_button = fo->index + 1; d->nfmt++;
        }
    }
    RET(0);
}
static void did_coop(CPU *c, ComObj *s) { Dev *d = D(s); uint32_t f = A(2); d->exclusive = (f & 1) != 0; d->foreground = (f & 4) != 0; RET(0); }
static void did_acquire(CPU *c, ComObj *s) { Dev *d = D(s); if (!d->fmt) { RET(DIERR_INVALIDPARAM); return; } int was = d->acquired; d->acquired = 1; d->dx = d->dy = d->dz = 0; mouse_mode(); RET(was ? DI_NOEFFECT : DI_OK); }
static void did_unacquire(CPU *c, ComObj *s) { Dev *d = D(s); int was = d->acquired; d->acquired = 0; mouse_mode(); RET(was ? DI_OK : DI_NOEFFECT); }
static void did_state(CPU *c, ComObj *s) {
    Dev *d = D(s); input_pump();
    if (!d->acquired) { RET(d->foreground ? DIERR_INPUTLOST : DIERR_NOTACQUIRED); return; }
    uint32_t n = A(1), p = A(2); if (!p || n < d->fmt_size) { RET(DIERR_INVALIDPARAM); return; }
    memset(GP(p), 0, n);
    for (int i = 0; i < d->nfmt; i++) {
        FmtObj *f = &d->fmt[i]; if (f->ofs >= n) continue;
        if (!d->mouse) { if (f->kind == 0 && f->index < 256) WR8(p + f->ofs, g_input.keys[f->index]); continue; }
        if (f->kind == 1 && f->ofs + 4 <= n) WR32(p + f->ofs, (uint32_t)(f->index == 0 ? (d->absolute ? g_input.mouse_x : d->dx) : f->index == 1 ? (d->absolute ? g_input.mouse_y : d->dy) : f->index == 2 ? d->dz : 0));
        if (f->kind == 0 && f->index < 5) WR8(p + f->ofs, g_input.buttons[f->index]);
    }
    if (d->mouse) d->dx = d->dy = d->dz = 0;
    RET(0);
}
static void did_data(CPU *c, ComObj *s) {
    Dev *d = D(s); input_pump();
    uint32_t sz = A(1), out = A(2), io = A(3), flags = A(4);
    if (!d->acquired) { RET(d->foreground ? DIERR_INPUTLOST : DIERR_NOTACQUIRED); return; }
    if (!io) { RET(DIERR_INVALIDPARAM); return; }
    uint32_t want = RD32(io), n = 0; int peek = (flags & 1) != 0;
    if (!d->qcap) { WR32(io, 0); RET(DIERR_NOTACQUIRED); return; }          /* DIERR_NOTBUFFERED really; games set the size first */
    while (n < want && n < d->qn) {
        DiEvent *e = &d->q[(d->qhead + n) % d->qcap];
        if (out && sz >= 16) { uint32_t o = out + n * sz; memset(GP(o), 0, sz); WR32(o, e->ofs); WR32(o + 4, e->data); WR32(o + 8, e->time); WR32(o + 12, e->seq); }
        n++;
    }
    if (!peek) { d->qhead = (d->qhead + n) % d->qcap; d->qn -= n; }
    WR32(io, n); uint32_t r = d->overflow ? DI_BUFFEROVERFLOW : DI_OK; if (!peek) d->overflow = 0; RET(r);
}
static void did_getprop(CPU *c, ComObj *s) {
    Dev *d = D(s); uint32_t prop = A(1), h = A(2);
    switch (prop) {
    case 1: WR32(h + 16, d->qcap); RET(0); return;                                    /* DIPROP_BUFFERSIZE */
    case 2: WR32(h + 16, d->absolute ? 1 : 0); RET(0); return;                        /* DIPROP_AXISMODE */
    case 3: WR32(h + 16, d->mouse && RD32(h + 8) == 8 ? 120 : 1); RET(0); return;     /* DIPROP_GRANULARITY: the wheel moves 120 */
    case 4: WR32(h + 16, 0x80000000u); WR32(h + 20, 0x7FFFFFFFu); RET(0); return;      /* DIPROP_RANGE */
    }
    RET(DIERR_UNSUPPORTED);
}
static void did_setprop(CPU *c, ComObj *s) {
    Dev *d = D(s); uint32_t prop = A(1), h = A(2);
    if (prop == 1) { uint32_t n = RD32(h + 16); if (n > 4096) n = 4096; free(d->q); d->q = n ? calloc(n, sizeof *d->q) : NULL; d->qcap = n; d->qn = d->qhead = 0; RET(0); return; }
    if (prop == 2) { d->absolute = RD32(h + 16) == 1; RET(0); return; }
    RET(DI_OK);
}
static void did_event(CPU *c, ComObj *s) { Dev *d = D(s); if (d->event) kobj_unref(d->event); d->event = A(1) ? handle_get(A(1), K_EVENT) : NULL; if (d->event) kobj_ref(d->event); RET(0); }
static void did_info(CPU *c, ComObj *s) { put_instance(A(1), D(s)->mouse); RET(0); }
static void did_poll(CPU *c, ComObj *s) { (void)s; input_pump(); RET(DI_NOEFFECT); }
static void did_enumobjects(CPU *c, ComObj *s) { (void)s; RET(0); }
static void did_objinfo(CPU *c, ComObj *s) { (void)s; RET(0x80070002u); }       /* DIERR_OBJECTNOTFOUND */
static void did_qi(CPU *c, ComObj *s) {
    static const uint8_t unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 }, a[16] = IID_IDirectInputDeviceA, a2[16] = IID_IDirectInputDevice2A, a7[16] = IID_IDirectInputDevice7A;
    uint32_t riid = A(1); int ok = com_iid_eq(riid, unk) || com_iid_eq(riid, a) || com_iid_eq(riid, a2) || com_iid_eq(riid, a7);   /* one object, the 7A vtable extends the others */
    if (ok) com_addref(s); OUTP(2, ok ? s->guest : 0); RET(ok ? 0 : E_NOINTERFACE_);
}
static void did_destroy(ComObj *o) {
    Dev *d = D(o); for (Dev **pp = &devs; *pp; pp = &(*pp)->next) if (*pp == d) { *pp = d->next; break; }
    if (d->event) kobj_unref(d->event); free(d->fmt); free(d->q); free(d); mouse_mode();
}
static const ComMethod did_m[] = { { "QueryInterface", did_qi }, { "GetCapabilities", did_caps }, { "EnumObjects", did_enumobjects }, { "GetProperty", did_getprop },
    { "SetProperty", did_setprop }, { "Acquire", did_acquire }, { "Unacquire", did_unacquire }, { "GetDeviceState", did_state }, { "GetDeviceData", did_data },
    { "SetDataFormat", did_setformat }, { "SetEventNotification", did_event }, { "SetCooperativeLevel", did_coop }, { "GetObjectInfo", did_objinfo },
    { "GetDeviceInfo", did_info }, { "Poll", did_poll }, { 0, 0 } };
ComClass c_did = { "IDirectInputDevice7A", IFACE_IDirectInputDevice7A, NULL, 0, did_m, DIERR_UNSUPPORTED, did_destroy };

/* ---------------------------------------------------------------- IDirectInput7A */
static ComObj *create_dev(uint32_t g) {
    int mouse = com_iid_eq(g, g_mouse); if (!mouse && !com_iid_eq(g, g_kbd)) return NULL;
    static int listening; if (!listening) { listening = 1; input_listen(on_input); }
    Dev *d = calloc(1, sizeof *d); d->mouse = mouse; d->next = devs; devs = d;
    return com_create(&c_did, d);
}
static void di_create(CPU *c, ComObj *s) { (void)s; ComObj *o = create_dev(A(1)); OUTP(2, o ? o->guest : 0); RET(o ? 0 : DIERR_DEVICENOTREG); }
static void di_enum(CPU *c, ComObj *s) {
    (void)s; uint32_t type = A(1) & 0xFF, cb = A(2), ref = A(3), inst = g_alloc(580);
    for (int mouse = 0; mouse < 2; mouse++) {
        if (type && type != (uint32_t)(mouse ? 2 : 3) && !(type == 1)) continue;         /* DIDEVTYPE_DEVICE 1 lists everything */
        WR32(inst, 580); put_instance(inst, mouse);
        if (!g_call(c, cb, 2, inst, ref)) break;
    }
    g_free(inst); RET(0);
}
static void di_status(CPU *c, ComObj *s) { (void)s; RET(com_iid_eq(A(1), g_kbd) || com_iid_eq(A(1), g_mouse) ? DI_OK : 1u); }
static void di_qi(CPU *c, ComObj *s) {
    static const uint8_t unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 }, a[16] = IID_IDirectInputA, a2[16] = IID_IDirectInput2A, a7[16] = IID_IDirectInput7A;
    uint32_t riid = A(1); int ok = com_iid_eq(riid, unk) || com_iid_eq(riid, a) || com_iid_eq(riid, a2) || com_iid_eq(riid, a7);
    if (ok) com_addref(s); OUTP(2, ok ? s->guest : 0); RET(ok ? 0 : E_NOINTERFACE_);
}
static void di_createex(CPU *c, ComObj *s) { (void)s; ComObj *o = create_dev(A(1)); OUTP(3, o ? o->guest : 0); RET(o ? 0 : DIERR_DEVICENOTREG); }   /* CreateDeviceEx(rguid, riid, ppv, outer) */
static void di_find(CPU *c, ComObj *s) { (void)s; RET(DIERR_DEVICENOTREG); }
static const ComMethod di_m[] = { { "QueryInterface", di_qi }, { "CreateDevice", di_create }, { "EnumDevices", di_enum }, { "GetDeviceStatus", di_status },
    { "FindDevice", di_find }, { "CreateDeviceEx", di_createex }, { 0, 0 } };
ComClass c_di = { "IDirectInput7A", IFACE_IDirectInput7A, NULL, 0, di_m, DI_OK, NULL };

SHIM(DirectInputCreateA) { if (!A(2)) { RET(DIERR_INVALIDPARAM); return; } WR32(A(2), com_create(&c_di, NULL)->guest); RET(0); }
SHIM(DirectInputCreateEx) { if (!A(3)) { RET(DIERR_INVALIDPARAM); return; } WR32(A(3), com_create(&c_di, NULL)->guest); RET(0); }
const ShimDef dinput_shims[] = {
    { "DirectInputCreateA", sh_DirectInputCreateA, STD(4) }, { "DirectInputCreateEx", sh_DirectInputCreateEx, STD(5) },
    { 0, 0, 0 }
};
