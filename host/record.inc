/* record.inc: included by win_host.c in builds made with `build_win.py --record` (-DRT_RECORD). The play-session recorder.
 *
 * While you play, it records for the engine's own functions (the ones in SS2.exe):
 *   - how often each ran, split by the "activity tag" that was active (see below),
 *   - up to four places it was called from,
 *   - up to three printable strings it was handed (file names, config variables, log text) on its first calls,
 * and for every place in the engine that calls into Windows / Direct3D / DirectSound / DirectInput:
 *   - which API function, how often, and up to four distinct string arguments (file names, registry keys, ...).
 * tools/suggest_names.py turns that, with the lifted code, into hints for naming functions (symbols/manual.sym).
 *
 * Activity tags: hold Ctrl+Alt and press F1..F11 to switch the tag (you hear a beep, higher pitch = higher tag), Ctrl+Alt+F12 goes back
 * to tag 0. Everything the game does afterwards is counted under that tag, so "I fired the pistol under tag 3" shows which functions
 * belong to firing. Write down what each tag meant; the report shows the numbers.
 *
 * Output: darkrecomp_record.txt next to the exe, rewritten about every 30 seconds and when the game exits. It also turns on the
 * Direct3D/DirectSound/DirectInput usage report (darkrecomp_api_usage.txt) for free. The recorder slows the game down a little. */

#define RC_TAGS 12
#define RC_FN_CAP (1 << 16)
#define RC_API_CAP (1 << 16)
#define RC_STR_CAP (1 << 15)
#define RC_NAME_CAP 4096
typedef struct { volatile uint32_t va; uint32_t first; uint32_t hits[RC_TAGS]; uint32_t caller[4]; int32_t str[3]; } RcFn;
typedef struct { volatile uint32_t site; uint32_t name; uint32_t n; int32_t str[4]; } RcApi;
static RcFn *rc_fn; static RcApi *rc_api;
static char (*rc_str)[64]; static volatile LONG rc_nstr;
static char (*rc_name)[56]; static volatile LONG rc_nname;
static volatile int rc_tag; static volatile LONG rc_seq, rc_order;
static DWORD rc_tag_ms[RC_TAGS], rc_tag_start, rc_t0, rc_last_write, rc_last_poll;
static int rc_ready;

static void rc_init(void) {
    rc_fn = (RcFn *)calloc(RC_FN_CAP, sizeof(RcFn)); rc_api = (RcApi *)calloc(RC_API_CAP, sizeof(RcApi));
    rc_str = (char (*)[64])calloc(RC_STR_CAP, 64); rc_name = (char (*)[56])calloc(RC_NAME_CAP, 56);
    if (!rc_fn || !rc_api || !rc_str || !rc_name) { hlog("RECOMP recorder: out of memory, off"); return; }
    rc_t0 = rc_tag_start = rc_last_write = rc_last_poll = GetTickCount(); rc_ready = 1;
    hlog("RECOMP play-session recorder ON: hold Ctrl+Alt and press F1..F11 to set the activity tag, F12 = tag 0; output darkrecomp_record.txt");
}

/* a printable C string at guest address p (identity-mapped: a host pointer)? copies it to out and returns its length, else 0 */
static int rc_cstr(uint32_t p, char *out, int cap) {
    if (p < 0x10000 || p > 0xFFFF0000u) return 0;
    char tmp[96]; size_t n = sizeof tmp;
    while (n && IsBadReadPtr((void *)(uintptr_t)p, n)) n >>= 1;
    if (n < 4) return 0;
    memcpy(tmp, (void *)(uintptr_t)p, n);
    int i = 0;
    for (; i < (int)n && tmp[i]; i++) { unsigned char ch = (unsigned char)tmp[i]; if (ch < 0x20 || ch > 0x7e) return 0; }
    if (i < 3 || i >= (int)n) return 0;
    if (i >= cap) i = cap - 1;
    for (int k = 0; k < i; k++) out[k] = tmp[k] == '|' ? '/' : tmp[k];
    out[i] = 0; return i;
}
/* interns a string, returns its index (or -1 when the pool is full) */
static int rc_intern(const char *s) {
    LONG n = rc_nstr; for (LONG i = 0; i < n && i < RC_STR_CAP; i++) if (!strcmp(rc_str[i], s)) return (int)i;
    LONG k = InterlockedIncrement(&rc_nstr) - 1; if (k >= RC_STR_CAP) return -1;
    snprintf(rc_str[k], 64, "%s", s); return (int)k;
}
static uint32_t rc_name_id(const char *s) {
    LONG n = rc_nname; for (LONG i = 0; i < n && i < RC_NAME_CAP; i++) if (!strcmp(rc_name[i], s)) return (uint32_t)i;
    LONG k = InterlockedIncrement(&rc_nname) - 1; if (k >= RC_NAME_CAP) return 0;
    snprintf(rc_name[k], 56, "%s", s); return (uint32_t)k;
}

static RcFn *rc_find(uint32_t va) {
    uint32_t h = (va * 2654435761u) >> 16;
    for (int i = 0; i < 64; i++) {
        RcFn *f = &rc_fn[(h + i) & (RC_FN_CAP - 1)];
        if (f->va == va) return f;
        if (!f->va) { if (InterlockedCompareExchange((volatile LONG *)&f->va, (LONG)va, 0) == 0 || f->va == va) { if (!f->first) f->first = (uint32_t)InterlockedIncrement(&rc_order); return f; } }
    }
    return NULL;
}
static void rc_poll(void);
/* called at the entry of every recompiled engine function (inserted by apply_hooks.py --record) */
void rec_enter(CPU *c, uint32_t va) {
    if (!rc_ready) return;
    RcFn *f = rc_find(va); if (!f) return;
    uint32_t h = ++f->hits[rc_tag];
    uint32_t ra = *(const uint32_t *)(uintptr_t)c->esp;
    for (int i = 0; i < 4; i++) { if (f->caller[i] == ra) break; if (!f->caller[i]) { f->caller[i] = ra; break; } }
    if (f->str[2] == 0 && h <= 6) {   /* early calls: look for strings in the registers and the first stack arguments */
        const uint32_t *a = (const uint32_t *)(uintptr_t)(c->esp + 4);
        uint32_t cand[7] = { c->ecx, c->edx, c->eax, a[0], a[1], a[2], a[3] };
        for (int q = 0; q < 7; q++) {
            char s[64]; if (!rc_cstr(cand[q], s, 64)) continue;
            int id = rc_intern(s); if (id < 0) continue;
            int dup = 0, slot = -1;
            for (int k = 0; k < 3; k++) { if (f->str[k] == id + 1) dup = 1; else if (!f->str[k] && slot < 0) slot = k; }
            if (!dup && slot >= 0) f->str[slot] = id + 1;
        }
    }
    if (!(++rc_seq & 0x3FFF)) rc_poll();
}

/* a call from engine code (return address `site`) into an API; `name` is "dll!function" or "Interface::Method" */
static void rc_api_call(uint32_t site, const char *name, const uint32_t *a) {
    if (!rc_ready) return;
    uint32_t nid = rc_name_id(name), h = (site * 2654435761u ^ nid * 40503u) >> 4;
    RcApi *e = NULL;
    for (int i = 0; i < 64; i++) {
        RcApi *t = &rc_api[(h + i) & (RC_API_CAP - 1)];
        if (t->site == site && t->name == nid) { e = t; break; }
        if (!t->site) { if (InterlockedCompareExchange((volatile LONG *)&t->site, (LONG)site, 0) == 0) { t->name = nid; e = t; break; } if (t->site == site && t->name == nid) { e = t; break; } }
    }
    if (!e) return;
    if (++e->n <= 12 && e->str[3] == 0) {
        for (int q = 0; q < 4; q++) {
            char s[64]; if (!rc_cstr(a[q], s, 64)) continue;
            int id = rc_intern(s); if (id < 0) continue;
            int dup = 0, slot = -1;
            for (int k = 0; k < 4; k++) { if (e->str[k] == id + 1) dup = 1; else if (!e->str[k] && slot < 0) slot = k; }
            if (!dup && slot >= 0) e->str[slot] = id + 1;
        }
    }
}

static void rc_write(void) {
    if (!rc_ready) return;
    DWORD now = GetTickCount(); rc_tag_ms[rc_tag] += now - rc_tag_start; rc_tag_start = now;
    char tmp[MAX_PATH], fin[MAX_PATH]; snprintf(fin, sizeof fin, "%s\\darkrecomp_record.txt", exe_dir); snprintf(tmp, sizeof tmp, "%s.tmp", fin);
    FILE *o = fopen(tmp, "w"); if (!o) return;
    fprintf(o, "# darkrecomp play-session record v1  seconds=%u presents=%u\n", (unsigned)((now - rc_t0) / 1000), au_presents);
    for (int t = 0; t < RC_TAGS; t++) fprintf(o, "tag %d %u\n", t, (unsigned)rc_tag_ms[t]);
    for (int i = 0; i < RC_FN_CAP; i++) {
        RcFn *f = &rc_fn[i]; if (!f->va) continue;
        fprintf(o, "fn %08x %u", f->va, f->first);
        for (int t = 0; t < RC_TAGS; t++) if (f->hits[t]) fprintf(o, " %d:%u", t, f->hits[t]);
        fprintf(o, " |");
        for (int k = 0; k < 4 && f->caller[k]; k++) fprintf(o, " %08x", f->caller[k]);
        fprintf(o, " |");
        for (int k = 0; k < 3 && f->str[k]; k++) fprintf(o, " %s%s", k ? "| " : "", rc_str[f->str[k] - 1]);
        fprintf(o, "\n");
    }
    for (int i = 0; i < RC_API_CAP; i++) {
        RcApi *e = &rc_api[i]; if (!e->site) continue;
        fprintf(o, "api %08x %s %u |", e->site, rc_name[e->name], e->n);
        for (int k = 0; k < 4 && e->str[k]; k++) fprintf(o, " %s%s", k ? "| " : "", rc_str[e->str[k] - 1]);
        fprintf(o, "\n");
    }
    fclose(o);
    MoveFileExA(tmp, fin, MOVEFILE_REPLACE_EXISTING);
    rc_last_write = now;
}

static void rc_poll(void) {
    DWORD now = GetTickCount();
    if (now - rc_last_poll < 40) return;
    rc_last_poll = now;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        for (int k = 0; k < 12; k++) {
            if (!(GetAsyncKeyState(VK_F1 + k) & 0x8000)) continue;
            int t = k == 11 ? 0 : k + 1;
            if (t != rc_tag) { rc_tag_ms[rc_tag] += now - rc_tag_start; rc_tag_start = now; rc_tag = t; hlog("RECOMP recorder: activity tag %d", t); Beep(300 + 120 * t, 60); }
        }
    }
    if (now - rc_last_write > 30000) rc_write();
}

/* from call_native: `fn` was reached through import `idx` (or a COM method when idx < 0 and au_pre named it) */
static void rc_native(int idx, const uint32_t *a) {
    if (!rc_ready) return;
    char nm[56]; uint32_t site = a[-1];
    if (idx >= 0) {
        snprintf(nm, sizeof nm, "%s!%s", hd_imports[idx].dll, hd_imports[idx].name);
        const char *f = hd_imports[idx].name;
        if (!strcmp(f, "ExitProcess") || !strcmp(f, "exit") || !strcmp(f, "_exit") || !strcmp(f, "TerminateProcess") || !strcmp(f, "abort")) { rc_api_call(site, nm, a); rc_write(); return; }
    } else if (au_cur_slot >= 0) { char b[24]; snprintf(nm, sizeof nm, "%s::%s", au_cur_iface, au_method(au_cur_iface, au_cur_slot, b, sizeof b)); }
    else return;
    rc_api_call(site, nm, a);
}
