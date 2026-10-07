/* com.c: the COM object model behind the DirectX front ends (see com.h). */
#include <stdio.h>
#include <stdlib.h>
#include "com.h"

#define MAGIC 0x214D4F43u        /* 'COM!' */
static ComObj **objs; static uint32_t nobjs, capobjs, free_hint;
static ComClass *classes[64]; static int nclasses;
static const uint8_t iid_unknown[16] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 };

int com_iid_eq(uint32_t g, const uint8_t iid[16]) { return g && g_valid(g, 16) && !memcmp(GP(g), iid, 16); }
ComObj *com_get(uint32_t g) {
    if (!g || !g_valid(g, 16) || RD32(g + 4) != MAGIC) return NULL;
    uint32_t i = RD32(g + 8); return i < nobjs && objs[i] && objs[i]->guest == g ? objs[i] : NULL;
}
ComObj *com_get_as(uint32_t g, ComClass *cls) { ComObj *o = com_get(g); return o && o->cls == cls ? o : NULL; }
void com_addref(ComObj *o) { o->refs++; }
void com_release(ComObj *o) {
    if (--o->refs > 0) return;
    if (o->cls->destroy) o->cls->destroy(o);
    uint32_t i = RD32(o->guest + 8); WR32(o->guest + 4, 0); g_free(o->guest); objs[i] = NULL; if (i < free_hint) free_hint = i; free(o);
}
static void dispatch(CPU *c) {
    ComClass *cls = classes[g_targ >> 8]; int slot = (int)(g_targ & 255);
    ComObj *self = com_get(A(0));
    if (!self) port_die("%s method %d called on %08x, which is not a live object (used after its last Release?)", cls->name, slot, A(0));
    com_method f = cls->slot_fn[slot];
    c->eax = cls->default_hr;
    if (f) { f(c, self); return; }
    if (slot == 0) {        /* QueryInterface */
        uint32_t riid = A(1), out = A(2); int ok = com_iid_eq(riid, iid_unknown);
        for (int i = 0; !ok && i < cls->niids; i++) ok = com_iid_eq(riid, cls->iids[i]);
        if (ok) { com_addref(self); if (out) WR32(out, self->guest); c->eax = 0; }
        else { if (out) WR32(out, 0); c->eax = E_NOINTERFACE_; if (riid && g_valid(riid, 16)) port_debug("%s::QueryInterface: interface %08x-... not available", cls->name, RD32(riid)); }
        return;
    }
    if (slot == 1) { com_addref(self); c->eax = (uint32_t)self->refs; return; }
    if (slot == 2) { int r = self->refs - 1; com_release(self); c->eax = (uint32_t)(r < 0 ? 0 : r); return; }
}
static void build(ComClass *cls) {
    if (cls->vtable) return;
    if (nclasses == 64) port_die("too many COM classes");
    cls->index = nclasses; classes[nclasses++] = cls;
    char *spec = xstrdup(cls->spec); int n = 0; for (char *p = spec; *p; p++) if (*p == ' ') n++; n++;
    cls->nslots = n; cls->slot_fn = calloc((size_t)n, sizeof *cls->slot_fn);
    uint32_t vt = g_alloc(4 * (uint32_t)n); int i = 0;
    for (char *tok = spec; tok && *tok; i++) {
        char *sp = strchr(tok, ' '); if (sp) *sp = 0;
        char *colon = strchr(tok, ':'); *colon = 0; int nargs = atoi(colon + 1);
        size_t l = strlen(cls->name) + strlen(tok) + 3; char *full = malloc(l); snprintf(full, l, "%s::%s", cls->name, tok);
        WR32(vt + 4 * (uint32_t)i, g_thunk_arg(dispatch, 4 * (nargs + 1), full, ((uint32_t)cls->index << 8) | (uint32_t)i));
        for (const ComMethod *m = cls->methods; m && m->name; m++) if (!strcmp(m->name, tok)) cls->slot_fn[i] = m->fn;
        tok = sp ? sp + 1 : NULL;
    }
    for (const ComMethod *m = cls->methods; m && m->name; m++) {     /* a typo in a method name would silently never be called */
        int found = 0; for (char *p = cls->spec ? strstr(cls->spec, m->name) : NULL; p; p = strstr(p + 1, m->name)) if ((p == cls->spec || p[-1] == ' ') && p[strlen(m->name)] == ':') found = 1;
        if (!found) port_die("%s has no method %s (check the name against ifaces.h)", cls->name, m->name);
    }
    free(spec); cls->vtable = vt;
}
ComObj *com_create(ComClass *cls, void *data) {
    build(cls);
    ComObj *o = calloc(1, sizeof *o); o->cls = cls; o->refs = 1; o->data = data;
    uint32_t i = free_hint; while (i < nobjs && objs[i]) i++;
    if (i == nobjs) { if (nobjs == capobjs) { capobjs = capobjs ? capobjs * 2 : 256; objs = realloc(objs, capobjs * sizeof *objs); } nobjs++; }
    objs[i] = o; free_hint = i + 1;
    o->guest = g_alloc(16); WR32(o->guest, cls->vtable); WR32(o->guest + 4, MAGIC); WR32(o->guest + 8, i);
    return o;
}
