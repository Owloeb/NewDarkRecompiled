/* user32.c: windows, the message queue and keyboard/mouse input on top of plat_video.
 *
 * The game creates one top-level window and drives everything through its window procedure and a PeekMessage loop. The
 * platform's single window stands in for it: platform events become the Windows messages the engine expects
 * (WM_KEYDOWN with the right virtual key and lParam, WM_CHAR, WM_MOUSEMOVE, WM_ACTIVATEAPP, WM_CLOSE...), and the
 * same events feed DirectInput (dx/dinput.c). Window procedures always run on the thread that pumps messages. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "core.h"

InputState g_input;
extern void kwait_set_msg_check(int (*fn)(void));

/* ---------------------------------------------------------------- scan codes and virtual keys */
static const uint8_t dik_vk[256] = {
    [0x01] = 0x1B, [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5', [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = 0xBD, [0x0D] = 0xBB, [0x0E] = 0x08, [0x0F] = 0x09, [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T', [0x15] = 'Y', [0x16] = 'U',
    [0x17] = 'I', [0x18] = 'O', [0x19] = 'P', [0x1A] = 0xDB, [0x1B] = 0xDD, [0x1C] = 0x0D, [0x1D] = 0x11, [0x1E] = 'A', [0x1F] = 'S', [0x20] = 'D', [0x21] = 'F',
    [0x22] = 'G', [0x23] = 'H', [0x24] = 'J', [0x25] = 'K', [0x26] = 'L', [0x27] = 0xBA, [0x28] = 0xDE, [0x29] = 0xC0, [0x2A] = 0x10, [0x2B] = 0xDC, [0x2C] = 'Z',
    [0x2D] = 'X', [0x2E] = 'C', [0x2F] = 'V', [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = 0xBC, [0x34] = 0xBE, [0x35] = 0xBF, [0x36] = 0x10, [0x37] = 0x6A,
    [0x38] = 0x12, [0x39] = 0x20, [0x3A] = 0x14, [0x3B] = 0x70, [0x3C] = 0x71, [0x3D] = 0x72, [0x3E] = 0x73, [0x3F] = 0x74, [0x40] = 0x75, [0x41] = 0x76, [0x42] = 0x77,
    [0x43] = 0x78, [0x44] = 0x79, [0x45] = 0x90, [0x46] = 0x91, [0x47] = 0x67, [0x48] = 0x68, [0x49] = 0x69, [0x4A] = 0x6D, [0x4B] = 0x64, [0x4C] = 0x65, [0x4D] = 0x66,
    [0x4E] = 0x6B, [0x4F] = 0x61, [0x50] = 0x62, [0x51] = 0x63, [0x52] = 0x60, [0x53] = 0x6E, [0x56] = 0xE2, [0x57] = 0x7A, [0x58] = 0x7B,
    [0x9C] = 0x0D, [0x9D] = 0x11, [0xB5] = 0x6F, [0xB7] = 0x2C, [0xB8] = 0x12, [0xC5] = 0x13, [0xC7] = 0x24, [0xC8] = 0x26, [0xC9] = 0x21, [0xCB] = 0x25,
    [0xCD] = 0x27, [0xCF] = 0x23, [0xD0] = 0x28, [0xD1] = 0x22, [0xD2] = 0x2D, [0xD3] = 0x2E, [0xDB] = 0x5B, [0xDC] = 0x5C, [0xDD] = 0x5D,
};
int dik_to_vk(int dik) { return dik >= 0 && dik < 256 ? dik_vk[dik] : 0; }
static int vk_down(int vk) {
    switch (vk) {
    case 0x01: return g_input.buttons[0]; case 0x02: return g_input.buttons[1]; case 0x04: return g_input.buttons[2]; case 0x05: return g_input.buttons[3]; case 0x06: return g_input.buttons[4];
    case 0x10: return g_input.keys[0x2A] || g_input.keys[0x36]; case 0x11: return g_input.keys[0x1D] || g_input.keys[0x9D]; case 0x12: return g_input.keys[0x38] || g_input.keys[0xB8];
    case 0xA0: return g_input.keys[0x2A]; case 0xA1: return g_input.keys[0x36]; case 0xA2: return g_input.keys[0x1D]; case 0xA3: return g_input.keys[0x9D]; case 0xA4: return g_input.keys[0x38]; case 0xA5: return g_input.keys[0xB8];
    }
    for (int i = 0; i < 256; i++) if (dik_vk[i] == vk && g_input.keys[i]) return 1;
    return 0;
}
static uint8_t key_toggled[256];

/* ---------------------------------------------------------------- windows */
typedef struct { char name[64]; uint32_t proc, style, extra; } WClass;
typedef struct { uint32_t hwnd, proc, style, exstyle, parent, id, userdata, menu; int x, y, w, h; char cls[64], text[256]; int alive, visible; uint32_t extra[16]; } Wnd;
static WClass classes[32]; static int nclasses;
static Wnd wins[16];
static uint32_t focus_hwnd, capture_hwnd, active_hwnd;
static int cursor_count;                 /* ShowCursor display counter: >= 0 shows the pointer */
static int clip_on;
static int di_acquired, di_exclusive, captured;   /* pointer capture: see apply_mouse_mode */
static Wnd *W(uint32_t h) { for (int i = 0; i < 16; i++) if (wins[i].alive && wins[i].hwnd == h) return &wins[i]; return NULL; }
static uint32_t wndcall(uint32_t h, uint32_t msg, uint32_t wp, uint32_t lp) {
    Wnd *w = W(h); if (!w || !w->proc) return 0;
    return g_call(cur_cpu(), w->proc, 4, h, msg, wp, lp);
}
static const char *class_name(uint32_t g) { static char b[16]; if (g < 0x10000) { snprintf(b, sizeof b, "#%u", g); return b; } return gs(g); }
static WClass *find_class(const char *n) { for (int i = 0; i < nclasses; i++) if (!strcmp(classes[i].name, n)) return &classes[i]; return NULL; }
SHIM(RegisterClassA) {
    uint32_t wc = A(0); const char *n = class_name(RD32(wc + 36)); WClass *k = find_class(n);
    if (!k) { if (nclasses == 32) { RET(0); return; } k = &classes[nclasses++]; }
    snprintf(k->name, sizeof k->name, "%s", n); k->style = RD32(wc); k->proc = RD32(wc + 4); k->extra = RD32(wc + 12);
    RET(0xC000 + (uint32_t)(k - classes));
}
SHIM(RegisterClassExA) {
    uint32_t wc = A(0); const char *n = class_name(RD32(wc + 40)); WClass *k = find_class(n);
    if (!k) { if (nclasses == 32) { RET(0); return; } k = &classes[nclasses++]; }
    snprintf(k->name, sizeof k->name, "%s", n); k->style = RD32(wc + 4); k->proc = RD32(wc + 8); k->extra = RD32(wc + 16);
    RET(0xC000 + (uint32_t)(k - classes));
}
SHIM(UnregisterClassA) { RET(1); }
static void open_platform_window(Wnd *w) {
    int ww = w->w > 0 ? w->w : 640, wh = w->h > 0 ? w->h : 480;
    if (plat_video_open(ww, wh, 0, w->text[0] ? w->text : "System Shock 2")) port_warn("the platform could not open a window");
    int cw, ch; plat_video_window_size(&cw, &ch); if (cw > 0 && ch > 0) { w->w = cw; w->h = ch; }
}
static void activate(Wnd *w) {
    if (!w || active_hwnd == w->hwnd) return;
    active_hwnd = focus_hwnd = w->hwnd; g_input.focused = 1;
    wndcall(w->hwnd, 0x1C, 1, 0);        /* WM_ACTIVATEAPP */
    wndcall(w->hwnd, 0x06, 1, 0);        /* WM_ACTIVATE (WA_ACTIVE) */
    wndcall(w->hwnd, 0x07, 0, 0);        /* WM_SETFOCUS */
}
SHIM(CreateWindowExA) {
    Wnd *w = NULL; for (int i = 0; i < 16; i++) if (!wins[i].alive) { w = &wins[i]; break; }
    WClass *k = find_class(class_name(A(1)));
    if (!w || !k) { set_last_error(k ? 8 : 1407); RET(0); return; }
    memset(w, 0, sizeof *w); w->alive = 1; w->hwnd = 0x10010u + 4u * (uint32_t)(w - wins);
    w->proc = k->proc; w->exstyle = A(0); w->style = A(3); w->x = (int32_t)A(4); w->y = (int32_t)A(5); w->w = (int32_t)A(6); w->h = (int32_t)A(7);
    if (w->x == (int32_t)0x80000000) w->x = 0; if (w->y == (int32_t)0x80000000) w->y = 0;
    if (w->w == (int32_t)0x80000000 || w->w <= 0) w->w = 640; if (w->h == (int32_t)0x80000000 || w->h <= 0) w->h = 480;
    w->parent = A(8); w->menu = A(9);
    snprintf(w->cls, sizeof w->cls, "%s", k->name); snprintf(w->text, sizeof w->text, "%s", gs(A(2)));
    int top = !w->parent || !(w->style & 0x40000000u);
    if (top && !g_input.hwnd) { g_input.hwnd = w->hwnd; open_platform_window(w); }
    /* CREATESTRUCTA, then the creation messages */
    uint32_t cs = g_alloc(48);
    WR32(cs, A(11)); WR32(cs + 4, A(10)); WR32(cs + 8, A(9)); WR32(cs + 12, A(8)); WR32(cs + 16, (uint32_t)w->h); WR32(cs + 20, (uint32_t)w->w);
    WR32(cs + 24, (uint32_t)w->y); WR32(cs + 28, (uint32_t)w->x); WR32(cs + 32, w->style); WR32(cs + 36, A(2)); WR32(cs + 40, A(1)); WR32(cs + 44, w->exstyle);
    uint32_t h = w->hwnd;
    if (!wndcall(h, 0x81, 0, cs)) { g_free(cs); w->alive = 0; if (g_input.hwnd == h) g_input.hwnd = 0; RET(0); return; }   /* WM_NCCREATE */
    if ((int32_t)wndcall(h, 0x01, 0, cs) == -1) { g_free(cs); w->alive = 0; if (g_input.hwnd == h) g_input.hwnd = 0; RET(0); return; }   /* WM_CREATE */
    g_free(cs);
    wndcall(h, 0x05, 0, ((uint32_t)w->h << 16) | (uint32_t)(w->w & 0xFFFF));          /* WM_SIZE */
    if (w->style & 0x10000000u) { w->visible = 1; activate(w); }                         /* WS_VISIBLE */
    RET(h);
}
SHIM(DestroyWindow) {
    uint32_t h = A(0); Wnd *w = W(h); if (!w) { RET(0); return; }
    wndcall(h, 0x02, 0, 0); wndcall(h, 0x82, 0, 0);       /* WM_DESTROY, WM_NCDESTROY */
    w->alive = 0; if (g_input.hwnd == h) g_input.hwnd = 0; if (active_hwnd == h) active_hwnd = 0; RET(1);
}
SHIM(ShowWindow) { Wnd *w = W(A(0)); int was = w && w->visible; if (w) { w->visible = A(1) != 0; if (w->visible) activate(w); } RET(was); }
SHIM(UpdateWindow) { RET(1); }
SHIM(SetForegroundWindow) { activate(W(A(0))); RET(1); }
SHIM(SetActiveWindow) { uint32_t o = active_hwnd; activate(W(A(0))); RET(o); }
SHIM(SetFocus) { uint32_t o = focus_hwnd; activate(W(A(0))); focus_hwnd = A(0); RET(o); }
SHIM(GetFocus) { RET(focus_hwnd); }
SHIM(GetActiveWindow) { RET(active_hwnd); }
SHIM(GetForegroundWindow) { RET(active_hwnd); }
SHIM(BringWindowToTop) { RET(1); }
SHIM(IsWindow) { RET(W(A(0)) != NULL); }
SHIM(IsWindowVisible) { Wnd *w = W(A(0)); RET(w && w->visible); }
SHIM(IsIconic) { RET(0); }
SHIM(IsZoomed) { RET(0); }
SHIM(GetParent) { Wnd *w = W(A(0)); RET(w ? w->parent : 0); }
SHIM(GetWindow) { RET(0); }
SHIM(GetDesktopWindow) { RET(0x10004); }
SHIM(EnableWindow) { RET(0); }
SHIM(GetClientRect) { Wnd *w = W(A(0)); uint32_t p = A(1); WR32(p, 0); WR32(p + 4, 0); WR32(p + 8, w ? (uint32_t)w->w : 0); WR32(p + 12, w ? (uint32_t)w->h : 0); RET(w != NULL); }
SHIM(GetWindowRect) {
    Wnd *w = W(A(0)); uint32_t p = A(1); int dw, dh; plat_video_display_size(&dw, &dh);
    if (A(0) == 0x10004) { WR32(p, 0); WR32(p + 4, 0); WR32(p + 8, (uint32_t)dw); WR32(p + 12, (uint32_t)dh); RET(1); return; }
    if (!w) { RET(0); return; } WR32(p, (uint32_t)w->x); WR32(p + 4, (uint32_t)w->y); WR32(p + 8, (uint32_t)(w->x + w->w)); WR32(p + 12, (uint32_t)(w->y + w->h)); RET(1);
}
static void resize(Wnd *w, int x, int y, int cw, int ch, int move, int size) {
    if (move) { w->x = x; w->y = y; }
    if (size && cw > 0 && ch > 0 && (cw != w->w || ch != w->h)) { w->w = cw; w->h = ch; wndcall(w->hwnd, 0x05, 0, ((uint32_t)ch << 16) | (uint32_t)(cw & 0xFFFF)); }
}
SHIM(MoveWindow) { Wnd *w = W(A(0)); if (w) resize(w, (int32_t)A(1), (int32_t)A(2), (int32_t)A(3), (int32_t)A(4), 1, 1); RET(w != NULL); }
SHIM(SetWindowPos) {
    Wnd *w = W(A(0)); if (!w) { RET(0); return; } uint32_t f = A(6);
    resize(w, (int32_t)A(2), (int32_t)A(3), (int32_t)A(4), (int32_t)A(5), !(f & 2), !(f & 1));
    if (f & 0x40) { w->visible = 1; activate(w); } if (f & 0x80) w->visible = 0;
    RET(1);
}
SHIM(AdjustWindowRect) { RET(1); }
SHIM(AdjustWindowRectEx) { RET(1); }
SHIM(SetWindowTextA) { Wnd *w = W(A(0)); if (w) { snprintf(w->text, sizeof w->text, "%s", gs(A(1))); if (w->hwnd == g_input.hwnd) plat_video_set_title(w->text); } RET(1); }
SHIM(GetWindowTextA) { Wnd *w = W(A(0)); if (!w || !A(2)) { RET(0); return; } uint32_t l = (uint32_t)strlen(w->text); if (l >= A(2)) l = A(2) - 1; memcpy(GP(A(1)), w->text, l); WR8(A(1) + l, 0); RET(l); }
static uint32_t *wlong(Wnd *w, int32_t idx) {
    switch (idx) { case -4: return &w->proc; case -16: return &w->style; case -20: return &w->exstyle; case -8: return &w->parent; case -12: return &w->id; case -21: return &w->userdata; }
    if (idx >= 0 && idx / 4 < 16) return &w->extra[idx / 4];
    return NULL;
}
SHIM(GetWindowLongA) { Wnd *w = W(A(0)); uint32_t *p = w ? wlong(w, (int32_t)A(1)) : NULL; RET(p ? *p : 0); }
SHIM(SetWindowLongA) { Wnd *w = W(A(0)); uint32_t *p = w ? wlong(w, (int32_t)A(1)) : NULL; if (!p) { RET(0); return; } uint32_t o = *p; *p = A(2); RET(o); }
SHIM(GetClassLongA) { RET(0); }
SHIM(SetClassLongA) { RET(0); }
SHIM(CallWindowProcA) { RET(A(0) ? g_call(c, A(0), 4, A(1), A(2), A(3), A(4)) : 0); }

/* ---------------------------------------------------------------- the message queue */
typedef struct { uint32_t hwnd, msg, wp, lp, time; int x, y; } QMsg;
#define QN 1024
static QMsg q[QN]; static unsigned qh, qt; static int quit_posted; static uint32_t quit_code;
static void post(uint32_t h, uint32_t m, uint32_t wp, uint32_t lp) {
    if (qt - qh >= QN) { if (m == 0x200) return; qh++; }          /* full: drop the oldest (never happens in practice) */
    q[qt++ % QN] = (QMsg){ h, m, wp, lp, ms_ticks(), g_input.mouse_x, g_input.mouse_y };
}
static void to_game_coords(int x, int y, int *gx, int *gy) {
    Wnd *w = W(g_input.hwnd); int cw, ch; plat_video_window_size(&cw, &ch);
    if (!w || cw <= 0 || ch <= 0) { *gx = x; *gy = y; return; }
    *gx = (int)((int64_t)x * w->w / cw); *gy = (int)((int64_t)y * w->h / ch);
    if (*gx < 0) *gx = 0; if (*gy < 0) *gy = 0; if (*gx >= w->w) *gx = w->w - 1; if (*gy >= w->h) *gy = w->h - 1;
}
static uint32_t mk_flags(void) {
    return (g_input.buttons[0] ? 1u : 0) | (g_input.buttons[1] ? 2u : 0) | (vk_down(0x10) ? 4u : 0) | (vk_down(0x11) ? 8u : 0) | (g_input.buttons[2] ? 0x10u : 0) | (g_input.buttons[3] ? 0x20u : 0) | (g_input.buttons[4] ? 0x40u : 0);
}
static input_listener listeners[4]; static int nlisteners;
void input_listen(input_listener fn) { if (nlisteners < 4) listeners[nlisteners++] = fn; }
static void on_event(const PlatEvent *e, void *user) {
    (void)user; uint32_t h = g_input.hwnd;
    switch (e->type) {
    case PLAT_EV_KEY: {
        int k = e->key & 0xFF, was = g_input.keys[k]; g_input.keys[k] = (uint8_t)(e->down ? 0x80 : 0);
        if (e->down && !was) key_toggled[k] ^= 1;
        int vk = dik_vk[k]; if (!vk) break;
        int alt = g_input.keys[0x38] || g_input.keys[0xB8], sys = alt || vk == 0x12 || (vk == 0x79);
        uint32_t lp = 1u | ((uint32_t)(k & 0x7F) << 16) | (k & 0x80 ? 1u << 24 : 0) | (alt ? 1u << 29 : 0) | (was ? 1u << 30 : 0) | (e->down ? 0 : 3u << 30);
        if (h) post(h, e->down ? (sys ? 0x104u : 0x100u) : (sys ? 0x105u : 0x101u), (uint32_t)vk, lp);
        break; }
    case PLAT_EV_TEXT: if (h && e->text) post(h, 0x102, e->text < 256 ? e->text : '?', 1); break;              /* WM_CHAR */
    case PLAT_EV_MOUSE_MOVE: {
        int gx, gy; to_game_coords(e->x, e->y, &gx, &gy);
        if (captured) {           /* the pointer does not move: the cursor the game reads follows the motion (games that recentre it, menu cursors) */
            Wnd *cw = W(g_input.hwnd); gx = g_input.mouse_x + e->dx; gy = g_input.mouse_y + e->dy;
            if (cw && cw->w > 0 && cw->h > 0) { gx = gx < 0 ? 0 : gx >= cw->w ? cw->w - 1 : gx; gy = gy < 0 ? 0 : gy >= cw->h ? cw->h - 1 : gy; }
        }
        g_input.mouse_x = gx; g_input.mouse_y = gy; g_input.mouse_dx += e->dx; g_input.mouse_dy += e->dy;
        if (h) post(h, 0x200, mk_flags(), ((uint32_t)(gy & 0xFFFF) << 16) | (uint32_t)(gx & 0xFFFF));
        break; }
    case PLAT_EV_MOUSE_BUTTON: {
        int b = e->button; if (b < 0 || b > 4) break; g_input.buttons[b] = (uint8_t)(e->down ? 0x80 : 0);
        static const uint32_t dn[5] = { 0x201, 0x204, 0x207, 0x20B, 0x20B }, up[5] = { 0x202, 0x205, 0x208, 0x20C, 0x20C };
        uint32_t wp = mk_flags() | (b >= 3 ? (uint32_t)(b - 2) << 16 : 0);
        if (h) post(h, e->down ? dn[b] : up[b], wp, ((uint32_t)(g_input.mouse_y & 0xFFFF) << 16) | (uint32_t)(g_input.mouse_x & 0xFFFF));
        break; }
    case PLAT_EV_MOUSE_WHEEL: g_input.wheel += e->dy; if (h) post(h, 0x20A, mk_flags() | ((uint32_t)(e->dy & 0xFFFF) << 16), ((uint32_t)(g_input.mouse_y & 0xFFFF) << 16) | (uint32_t)(g_input.mouse_x & 0xFFFF)); break;
    case PLAT_EV_FOCUS:
        g_input.focused = e->down;
        if (!e->down) { memset(g_input.keys, 0, sizeof g_input.keys); memset(g_input.buttons, 0, sizeof g_input.buttons); }
        if (h) { post(h, 0x1C, (uint32_t)e->down, 0); post(h, 0x06, (uint32_t)e->down, 0); post(h, e->down ? 0x07 : 0x08, 0, 0); }
        break;
    case PLAT_EV_RESIZE: break;          /* the backend scales the game's picture; the game keeps its resolution */
    case PLAT_EV_QUIT: if (h) post(h, 0x10, 0, 0); else port_exit(0); break;             /* WM_CLOSE */
    default: break;
    }
    for (int i = 0; i < nlisteners; i++) listeners[i](e);
}
void input_pump(void) { plat_video_poll(on_event, NULL); }
static int msg_waiting(void) { input_pump(); return qt != qh || quit_posted; }
static int match(const QMsg *m, uint32_t h, uint32_t lo, uint32_t hi) { return (!h || m->hwnd == h) && ((!lo && !hi) || (m->msg >= lo && m->msg <= hi)); }
/* takes (remove) or looks at the first queued message that matches into the guest MSG at p */
static int peek(uint32_t p, uint32_t h, uint32_t lo, uint32_t hi, int remove) {
    for (unsigned i = qh; i != qt; i++) {
        QMsg *m = &q[i % QN]; if (!match(m, h, lo, hi)) continue;
        WR32(p, m->hwnd); WR32(p + 4, m->msg); WR32(p + 8, m->wp); WR32(p + 12, m->lp); WR32(p + 16, m->time); WR32(p + 20, (uint32_t)m->x); WR32(p + 24, (uint32_t)m->y);
        if (remove) { for (unsigned j = i; j != qh; j--) q[j % QN] = q[(j - 1) % QN]; qh++; }
        return 1;
    }
    if (quit_posted && (!lo || (lo <= 0x12 && hi >= 0x12))) {
        WR32(p, 0); WR32(p + 4, 0x12); WR32(p + 8, quit_code); WR32(p + 12, 0); WR32(p + 16, ms_ticks()); if (remove) quit_posted = 0; return 1;
    }
    return 0;
}
static void ensure_hook(void) { static int done; if (!done) { done = 1; kwait_set_msg_check(msg_waiting); } }
SHIM(PeekMessageA) { ensure_hook(); input_pump(); RET(peek(A(0), A(1), A(2), A(3), A(4) & 1)); }
SHIM(GetMessageA) {
    ensure_hook();
    for (;;) {
        input_pump();
        if (peek(A(0), A(1), A(2), A(3), 1)) { RET(RD32(A(0) + 4) != 0x12); return; }
        KObj *none[1]; kwait(none, 0, 0, 0xFFFFFFFFu, 1);
    }
}
SHIM(WaitMessage) { ensure_hook(); KObj *none[1]; if (!msg_waiting()) kwait(none, 0, 0, 0xFFFFFFFFu, 1); RET(1); }
SHIM(MsgWaitForMultipleObjects) {
    ensure_hook(); uint32_t n = A(0); KObj *o[64]; if (n > 64) n = 64;
    for (uint32_t i = 0; i < n; i++) { o[i] = handle_get(RD32(A(1) + 4 * i), K_NONE); if (!o[i] || !o[i]->signaled) { set_last_error(6); RET(0xFFFFFFFFu); return; } }
    RET(kwait(o, (int)n, A(2) != 0, A(3), A(4) != 0));
}
SHIM(TranslateMessage) { RET(0); }          /* WM_CHAR comes from the platform's text events */
SHIM(DispatchMessageA) { uint32_t p = A(0); RET(RD32(p) ? wndcall(RD32(p), RD32(p + 4), RD32(p + 8), RD32(p + 12)) : 0); }
SHIM(PostMessageA) { if (A(0) && !W(A(0))) { RET(0); return; } post(A(0) ? A(0) : g_input.hwnd, A(1), A(2), A(3)); RET(1); }
SHIM(PostThreadMessageA) { post(g_input.hwnd, A(1), A(2), A(3)); RET(1); }
SHIM(SendMessageA) { RET(wndcall(A(0), A(1), A(2), A(3))); }
SHIM(PostQuitMessage) { quit_posted = 1; quit_code = A(0); }
SHIM(GetMessageTime) { RET(ms_ticks()); }
SHIM(GetMessagePos) { RET(((uint32_t)(g_input.mouse_y & 0xFFFF) << 16) | (uint32_t)(g_input.mouse_x & 0xFFFF)); }
SHIM(GetQueueStatus) { input_pump(); RET(qt != qh ? 0x00FF00FFu : 0); }
SHIM(DefWindowProcA) {
    uint32_t h = A(0), m = A(1);
    switch (m) {
    case 0x81: RET(1); return;                                      /* WM_NCCREATE */
    case 0x10: { uint32_t args[1] = { h }; (void)args; Wnd *w = W(h); if (w) { wndcall(h, 0x02, 0, 0); wndcall(h, 0x82, 0, 0); w->alive = 0; if (g_input.hwnd == h) g_input.hwnd = 0; } RET(0); return; }   /* WM_CLOSE */
    case 0x14: RET(1); return;                                      /* WM_ERASEBKGND */
    case 0x112: if ((A(2) & 0xFFF0) == 0xF060) post(h, 0x10, 0, 0); RET(0); return;        /* WM_SYSCOMMAND SC_CLOSE */
    case 0x20: RET(0); return;                                      /* WM_SETCURSOR */
    case 0x84: RET(1); return;                                      /* WM_NCHITTEST: HTCLIENT */
    default: RET(0);
    }
}
SHIM(MessageBoxA) {
    port_log("MessageBox \"%s\": %s", gs(A(2)), gs(A(1)));
    uint32_t t = A(3) & 0xF; RET(t == 4 || t == 3 ? 6 : t == 5 ? 4 : 1);     /* IDYES / IDRETRY / IDOK */
}
SHIM(DialogBoxParamA) { port_log("DialogBoxParam: dialogs are not available on this host"); RET(0xFFFFFFFFu); }
SHIM(EndDialog) { RET(1); }
SHIM(GetDlgItem) { RET(0); }
SHIM(IsDlgButtonChecked) { RET(0); }
SHIM(BeginPaint) { uint32_t p = A(1); memset(GP(p), 0, 64); WR32(p, 0x30001); Wnd *w = W(A(0)); if (w) { WR32(p + 16, (uint32_t)w->w); WR32(p + 20, (uint32_t)w->h); } RET(0x30001); }
SHIM(EndPaint) { RET(1); }
SHIM(InvalidateRect) { RET(1); }
SHIM(ValidateRect) { RET(1); }
SHIM(GetDC) { RET(0x30001); }
SHIM(GetWindowDC) { RET(0x30001); }
SHIM(ReleaseDC) { RET(1); }

/* ---------------------------------------------------------------- keyboard and mouse */
SHIM(GetKeyState) { int vk = (int)(A(0) & 0xFF); uint16_t r = (uint16_t)(vk_down(vk) ? 0x8000 : 0); for (int i = 0; i < 256; i++) if (dik_vk[i] == vk && key_toggled[i]) { r |= 1; break; } RET((uint32_t)(int32_t)(int16_t)r); }
SHIM(GetAsyncKeyState) { input_pump(); RET(vk_down((int)(A(0) & 0xFF)) ? 0xFFFF8000u : 0); }
SHIM(GetKeyboardState) { input_pump(); for (int vk = 0; vk < 256; vk++) WR8(A(0) + (uint32_t)vk, vk_down(vk) ? 0x80 : 0); RET(1); }
SHIM(MapVirtualKeyA) {
    uint32_t code = A(0), type = A(1);
    if (type == 0 || type == 3) { for (int i = 0; i < 256; i++) if (dik_vk[i] == code) { RET(i & 0x7F); return; } RET(0); return; }   /* VK -> scan */
    if (type == 1) { RET(dik_vk[code & 0xFF]); return; }                                                                        /* scan -> VK */
    if (type == 2) { RET(code >= 'A' && code <= 'Z' ? code : code >= '0' && code <= '9' ? code : code == 0x20 ? 0x20 : 0); return; }  /* VK -> char */
    RET(0);
}
SHIM(GetKeyNameTextA) {
    int sc = (int)((A(0) >> 16) & 0x7F) | (A(0) & (1u << 24) ? 0x80 : 0), vk = dik_vk[sc]; char b[32];
    if (vk >= '0' && vk <= 'Z') snprintf(b, sizeof b, "%c", vk); else if (vk >= 0x70 && vk <= 0x7B) snprintf(b, sizeof b, "F%d", vk - 0x6F); else snprintf(b, sizeof b, "Key %02X", sc);
    uint32_t l = (uint32_t)strlen(b); if (!A(2)) { RET(0); return; } if (l >= A(2)) l = A(2) - 1; memcpy(GP(A(1)), b, l); WR8(A(1) + l, 0); RET(l);
}
SHIM(GetKeyboardType) { RET(A(0) == 0 ? 4 : A(0) == 2 ? 12 : 0); }
SHIM(GetKeyboardLayout) { RET(0x04090409u); }
SHIM(VkKeyScanA) { int ch = (int)(A(0) & 0xFF); RET(isalpha(ch) ? (uint32_t)(toupper(ch) | (isupper(ch) ? 0x100 : 0)) : isdigit(ch) ? (uint32_t)ch : ch == ' ' ? 0x20 : 0xFFFFFFFFu); }
SHIM(GetCursorPos) { input_pump(); Wnd *w = W(g_input.hwnd); WR32(A(0), (uint32_t)(g_input.mouse_x + (w ? w->x : 0))); WR32(A(0) + 4, (uint32_t)(g_input.mouse_y + (w ? w->y : 0))); RET(1); }
SHIM(SetCursorPos) {
    Wnd *w = W(g_input.hwnd); int x = (int32_t)A(0) - (w ? w->x : 0), y = (int32_t)A(1) - (w ? w->y : 0);
    g_input.mouse_x = x; g_input.mouse_y = y;
    int cw, ch; plat_video_window_size(&cw, &ch); if (w && w->w > 0 && w->h > 0 && cw > 0) plat_video_warp_mouse((int)((int64_t)x * cw / w->w), (int)((int64_t)y * ch / w->h));
    RET(1);
}
/* Capture the pointer (relative motion, no screen edges) when the game hid the cursor and reads the mouse itself:
   DirectInput (any cooperative level) or a confined cursor. DirectInput in exclusive mode always captures.
   On Windows, DirectInput reads the device, not the cursor, so a free pointer would stop at the screen edge. */
static void apply_mouse_mode(void) {
    captured = di_exclusive || (cursor_count < 0 && (di_acquired || clip_on));
    plat_video_mouse_mode(captured, cursor_count >= 0 && !captured);
}
void input_set_di_mouse(int acquired, int exclusive) {
    if (acquired == di_acquired && exclusive == di_exclusive) return;
    di_acquired = acquired; di_exclusive = exclusive; apply_mouse_mode();
}
SHIM(ShowCursor) { cursor_count += A(0) ? 1 : -1; apply_mouse_mode(); RET((uint32_t)cursor_count); }
SHIM(ClipCursor) { clip_on = A(0) != 0; apply_mouse_mode(); RET(1); }
SHIM(SetCapture) { uint32_t o = capture_hwnd; capture_hwnd = A(0); RET(o); }
SHIM(GetCapture) { RET(capture_hwnd); }
SHIM(ReleaseCapture) { capture_hwnd = 0; RET(1); }
SHIM(SetCursor) { RET(0); }
SHIM(LoadCursorA) { RET(0x40002); }
SHIM(LoadIconA) { RET(0x40001); }
SHIM(LoadImageA) { RET(0x40003); }
SHIM(ScreenToClient) { Wnd *w = W(A(0)); if (w) { WR32(A(1), RD32(A(1)) - (uint32_t)w->x); WR32(A(1) + 4, RD32(A(1) + 4) - (uint32_t)w->y); } RET(1); }
SHIM(ClientToScreen) { Wnd *w = W(A(0)); if (w) { WR32(A(1), RD32(A(1)) + (uint32_t)w->x); WR32(A(1) + 4, RD32(A(1) + 4) + (uint32_t)w->y); } RET(1); }
SHIM(MapWindowPoints) { RET(0); }
SHIM(GetSystemMetrics) {
    int dw, dh; plat_video_display_size(&dw, &dh);
    switch (A(0)) { case 0: case 16: case 78: RET((uint32_t)dw); return; case 1: case 17: case 79: RET((uint32_t)dh); return; case 80: RET(1); return;
                    case 19: RET(1); return; case 43: RET(3); return; case 4: RET(20); return; case 5: case 6: case 32: case 33: RET(4); return; default: RET(0); }
}
SHIM(SystemParametersInfoA) { if (A(0) == 0x30 && A(2)) { int dw, dh; plat_video_display_size(&dw, &dh); WR32(A(2), 0); WR32(A(2) + 4, 0); WR32(A(2) + 8, (uint32_t)dw); WR32(A(2) + 12, (uint32_t)dh); } RET(1); }
SHIM(GetMonitorInfoA) { int dw, dh; plat_video_display_size(&dw, &dh); uint32_t p = A(1); WR32(p + 4, 0); WR32(p + 8, 0); WR32(p + 12, (uint32_t)dw); WR32(p + 16, (uint32_t)dh); WR32(p + 20, 0); WR32(p + 24, 0); WR32(p + 28, (uint32_t)dw); WR32(p + 32, (uint32_t)dh); WR32(p + 36, 1); RET(1); }
SHIM(MonitorFromWindow) { RET(0x50001); }
SHIM(EnumDisplaySettingsA) {
    static const int modes[][2] = { { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 1280, 1024 }, { 1600, 900 }, { 1920, 1080 } };
    uint32_t i = A(1), p = A(2); int dw, dh; plat_video_display_size(&dw, &dh); int w, h;
    if (i == 0xFFFFFFFFu || i == 0xFFFFFFFEu) { w = dw; h = dh; } else if (i < 7) { w = modes[i][0]; h = modes[i][1]; } else { RET(0); return; }
    WR32(p + 104, 32); WR32(p + 108, (uint32_t)w); WR32(p + 112, (uint32_t)h); WR32(p + 120, 60); WR32(p + 40, 0x5C0000); RET(1);
}
SHIM(ChangeDisplaySettingsA) { RET(0); }
/* rectangles */
static int32_t R(uint32_t p, int i) { return (int32_t)RD32(p + 4 * (uint32_t)i); }
SHIM(IsRectEmpty) { uint32_t p = A(0); RET(R(p, 2) <= R(p, 0) || R(p, 3) <= R(p, 1)); }
SHIM(PtInRect) { uint32_t r = A(0); int32_t x = (int32_t)A(1), y = (int32_t)A(2); RET(x >= R(r, 0) && x < R(r, 2) && y >= R(r, 1) && y < R(r, 3)); }
SHIM(SetRect) { WR32(A(0), A(1)); WR32(A(0) + 4, A(2)); WR32(A(0) + 8, A(3)); WR32(A(0) + 12, A(4)); RET(1); }
SHIM(SetRectEmpty) { memset(GP(A(0)), 0, 16); RET(1); }
SHIM(CopyRect) { memcpy(GP(A(0)), GP(A(1)), 16); RET(1); }
SHIM(OffsetRect) { uint32_t p = A(0); int32_t dx = (int32_t)A(1), dy = (int32_t)A(2); WR32(p, (uint32_t)(R(p, 0) + dx)); WR32(p + 4, (uint32_t)(R(p, 1) + dy)); WR32(p + 8, (uint32_t)(R(p, 2) + dx)); WR32(p + 12, (uint32_t)(R(p, 3) + dy)); RET(1); }
SHIM(IntersectRect) {
    uint32_t d = A(0), a = A(1), b = A(2);
    int32_t l = R(a, 0) > R(b, 0) ? R(a, 0) : R(b, 0), t = R(a, 1) > R(b, 1) ? R(a, 1) : R(b, 1), r = R(a, 2) < R(b, 2) ? R(a, 2) : R(b, 2), bt = R(a, 3) < R(b, 3) ? R(a, 3) : R(b, 3);
    if (r <= l || bt <= t) { memset(GP(d), 0, 16); RET(0); return; } WR32(d, (uint32_t)l); WR32(d + 4, (uint32_t)t); WR32(d + 8, (uint32_t)r); WR32(d + 12, (uint32_t)bt); RET(1);
}
SHIM(UnionRect) {
    uint32_t d = A(0), a = A(1), b = A(2);
    int32_t l = R(a, 0) < R(b, 0) ? R(a, 0) : R(b, 0), t = R(a, 1) < R(b, 1) ? R(a, 1) : R(b, 1), r = R(a, 2) > R(b, 2) ? R(a, 2) : R(b, 2), bt = R(a, 3) > R(b, 3) ? R(a, 3) : R(b, 3);
    WR32(d, (uint32_t)l); WR32(d + 4, (uint32_t)t); WR32(d + 8, (uint32_t)r); WR32(d + 12, (uint32_t)bt); RET(1);
}
SHIM(GetSysColor) { RET(0); }
SHIM(GetMenu) { RET(0); }
SHIM(DestroyMenu) { RET(1); }
SHIM(SetMenu) { RET(1); }
SHIM(OpenClipboard) { RET(0); }
SHIM(CloseClipboard) { RET(1); }
SHIM(EmptyClipboard) { RET(1); }
SHIM(GetClipboardData) { RET(0); }
SHIM(IsClipboardFormatAvailable) { RET(0); }
SHIM(WinHelpA) { RET(1); }
SHIM(SetTimer) { RET(0); }
SHIM(KillTimer) { RET(1); }
SHIM(Ret1) { RET(1); }

const ShimDef user32_shims[] = {
    S(RegisterClassA, STD(1)), S(RegisterClassExA, STD(1)), S(UnregisterClassA, STD(2)), S(CreateWindowExA, STD(12)), S(DestroyWindow, STD(1)), S(ShowWindow, STD(2)),
    S(UpdateWindow, STD(1)), S(SetForegroundWindow, STD(1)), S(SetActiveWindow, STD(1)), S(SetFocus, STD(1)), S(GetFocus, 0), S(GetActiveWindow, 0), S(GetForegroundWindow, 0),
    S(BringWindowToTop, STD(1)), S(IsWindow, STD(1)), S(IsWindowVisible, STD(1)), S(IsIconic, STD(1)), S(IsZoomed, STD(1)), S(GetParent, STD(1)), S(GetWindow, STD(2)),
    S(GetDesktopWindow, 0), S(EnableWindow, STD(2)), S(GetClientRect, STD(2)), S(GetWindowRect, STD(2)), S(MoveWindow, STD(6)), S(SetWindowPos, STD(7)), S(AdjustWindowRect, STD(3)),
    S(AdjustWindowRectEx, STD(4)), S(SetWindowTextA, STD(2)), S(GetWindowTextA, STD(3)), S(GetWindowLongA, STD(2)), S(SetWindowLongA, STD(3)), S(GetClassLongA, STD(2)), S(SetClassLongA, STD(3)),
    S(CallWindowProcA, STD(5)),
    S(PeekMessageA, STD(5)), S(GetMessageA, STD(4)), S(WaitMessage, 0), S(MsgWaitForMultipleObjects, STD(5)), S(TranslateMessage, STD(1)), S(DispatchMessageA, STD(1)),
    S(PostMessageA, STD(4)), S(PostThreadMessageA, STD(4)), S(SendMessageA, STD(4)), S(PostQuitMessage, STD(1)), S(GetMessageTime, 0), S(GetMessagePos, 0), S(GetQueueStatus, STD(1)),
    S(DefWindowProcA, STD(4)), S(MessageBoxA, STD(4)), S(DialogBoxParamA, STD(5)), S(EndDialog, STD(2)), S(GetDlgItem, STD(2)), S(IsDlgButtonChecked, STD(2)),
    S(BeginPaint, STD(2)), S(EndPaint, STD(2)), S(InvalidateRect, STD(3)), S(ValidateRect, STD(2)), S(GetDC, STD(1)), S(GetWindowDC, STD(1)), S(ReleaseDC, STD(2)),
    S(GetKeyState, STD(1)), S(GetAsyncKeyState, STD(1)), S(GetKeyboardState, STD(1)), S(MapVirtualKeyA, STD(2)), S(GetKeyNameTextA, STD(3)), S(GetKeyboardType, STD(1)),
    S(GetKeyboardLayout, STD(1)), S(VkKeyScanA, STD(1)), S(GetCursorPos, STD(1)), S(SetCursorPos, STD(2)), S(ShowCursor, STD(1)), S(ClipCursor, STD(1)), S(SetCapture, STD(1)),
    S(GetCapture, 0), S(ReleaseCapture, 0), S(SetCursor, STD(1)), S(LoadCursorA, STD(2)), S(LoadIconA, STD(2)), S(LoadImageA, STD(6)), S(ScreenToClient, STD(2)), S(ClientToScreen, STD(2)),
    S(MapWindowPoints, STD(4)), S(GetSystemMetrics, STD(1)), S(SystemParametersInfoA, STD(4)), S(GetMonitorInfoA, STD(2)), S(MonitorFromWindow, STD(2)),
    S(EnumDisplaySettingsA, STD(3)), S(ChangeDisplaySettingsA, STD(2)),
    S(IsRectEmpty, STD(1)), S(PtInRect, STD(3)), S(SetRect, STD(5)), S(SetRectEmpty, STD(1)), S(CopyRect, STD(2)), S(OffsetRect, STD(3)), S(IntersectRect, STD(3)), S(UnionRect, STD(3)),
    S(GetSysColor, STD(1)), SA("SetSysColors", Ret1, STD(3)), S(GetMenu, STD(1)), S(DestroyMenu, STD(1)), S(SetMenu, STD(2)), S(OpenClipboard, STD(1)), S(CloseClipboard, 0),
    S(EmptyClipboard, 0), S(GetClipboardData, STD(1)), S(IsClipboardFormatAvailable, STD(1)), S(WinHelpA, STD(4)), S(SetTimer, STD(4)), S(KillTimer, STD(2)),
    SA("FillRect", Ret1, STD(3)),
    { 0, 0, 0 }
};
