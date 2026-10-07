/* dsound.c: DirectSound front end with a software mixer.
 *
 * Sound buffers keep their samples in guest memory, where the game writes them through Lock. The mixer runs in the
 * backend's audio callback (plat_audio_open), resamples every playing buffer to the output rate with linear
 * interpolation, applies volume, pan and a simple 3D model (distance attenuation between the minimum and maximum
 * distance, equal-power panning relative to the listener), and advances each buffer's play cursor by what was really
 * played, so GetCurrentPosition, GetStatus and position notifications behave as on Windows. When the platform has no
 * audio device, a timer thread drives the same mixer so the game still sees its buffers play. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "com.h"

#define DS_OK 0u
#define DSERR_INVALIDPARAM 0x80070057u
#define DSERR_CONTROLUNAVAIL 0x8878001Eu
#define DSERR_PRIOLEVELNEEDED 0x88780046u
#define OUT_RATE 44100
KObj *handle_get(uint32_t h, KType type);

typedef struct { float x, y, z; } V3;
typedef struct { int refs; uint32_t mem, size; } Data;      /* duplicated buffers share their samples */
typedef struct Buf {
    struct Buf *next; ComObj *obj; int primary;
    Data *data; uint32_t flags;
    uint16_t tag, channels, bits, align; uint32_t rate, avg;      /* format */
    uint32_t freq; int32_t volume, pan;
    double pos;                     /* play position in sample frames */
    int playing, looping;
    /* 3D */
    V3 p3, v3; float min_d, max_d; uint32_t mode3d; uint32_t cone_in, cone_out; V3 cone_dir; int32_t cone_vol;
    /* notifications */
    uint32_t nnotify; struct { uint32_t off; KObj *ev; } *notify;   /* events are referenced, not looked up by handle, so the audio thread never touches the handle table */
    uint32_t last_byte;
} Buf;
typedef struct { V3 pos, vel, front, top; float dist_f, rolloff, doppler; } Listener;
static PlatMutex *mx; static Buf *bufs; static Listener lis = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 }, 1, 1, 1 };
static int out_rate, started; static int32_t master_vol;
extern ComClass c_ds, c_dsb, c_ds3b, c_ds3l, c_notify, c_ksp;
static Buf *B(ComObj *o) { ComObj *m = o->extra_iface_of ? o->extra_iface_of : o; return (Buf *)m->data; }
static void lock(void) { plat_mutex_lock(mx); }
static void unlock(void) { plat_mutex_unlock(mx); }

/* ---------------------------------------------------------------- the mixer (audio thread) */
static float gain_db(int32_t mb) { return mb <= -10000 ? 0.0f : powf(10.0f, (float)mb / 2000.0f); }
static uint32_t frame_bytes(const Buf *b) { return b->align ? b->align : (uint32_t)(b->channels * b->bits / 8); }
static void sample_at(const Buf *b, const uint8_t *mem, uint32_t frames, uint32_t f, float *l, float *r) {
    if (f >= frames) f %= frames;
    const uint8_t *p = mem + (size_t)f * frame_bytes(b);
    if (b->bits == 16) { int16_t a, c; memcpy(&a, p, 2); c = a; if (b->channels > 1) memcpy(&c, p + 2, 2); *l = a / 32768.0f; *r = c / 32768.0f; }
    else { *l = (p[0] - 128) / 128.0f; *r = b->channels > 1 ? (p[1] - 128) / 128.0f : *l; }
}
static void notify_range(Buf *b, uint32_t from, uint32_t to, int wrapped) {    /* byte offsets passed by the play cursor */
    for (uint32_t i = 0; i < b->nnotify; i++) { uint32_t off = b->notify[i].off; if (off == 0xFFFFFFFFu || !b->notify[i].ev) continue;
        if ((!wrapped && off >= from && off < to) || (wrapped && (off >= from || off < to))) kevent_set(b->notify[i].ev); }
}
static void notify_stop(Buf *b) { for (uint32_t i = 0; i < b->nnotify; i++) if (b->notify[i].off == 0xFFFFFFFFu && b->notify[i].ev) kevent_set(b->notify[i].ev); }
static void drop_notify(Buf *b) { for (uint32_t i = 0; i < b->nnotify; i++) if (b->notify[i].ev) kobj_unref(b->notify[i].ev); free(b->notify); b->notify = NULL; b->nnotify = 0; }
static void gains_3d(const Buf *b, float *gl, float *gr) {
    *gl = *gr = 1.0f;
    if (!(b->flags & 0x10) || b->mode3d == 2) return;                 /* not 3D, or DS3DMODE_DISABLE */
    V3 rel = b->p3; if (b->mode3d != 1) { rel.x -= lis.pos.x; rel.y -= lis.pos.y; rel.z -= lis.pos.z; }
    float dist = sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z) * lis.dist_f;
    float mn = b->min_d > 0 ? b->min_d : 1, mx_ = b->max_d > mn ? b->max_d : 1e9f, dd = dist < mn ? mn : dist > mx_ ? mx_ : dist;
    float att = powf(mn / dd, lis.rolloff >= 0 ? lis.rolloff : 1);       /* rolloff 0 (allowed) means no distance attenuation */
    V3 f = lis.front, t = lis.top, right = { t.y * f.z - t.z * f.y, t.z * f.x - t.x * f.z, t.x * f.y - t.y * f.x };
    float rl = sqrtf(right.x * right.x + right.y * right.y + right.z * right.z), pan = 0;
    if (dist > 1e-4f && rl > 0) { pan = (rel.x * right.x + rel.y * right.y + rel.z * right.z) / (rl * sqrtf(rel.x * rel.x + rel.y * rel.y + rel.z * rel.z)); }
    if (!(pan > -1.0f)) pan = -1.0f; else if (pan > 1.0f) pan = 1.0f;          /* rounding (or a NaN position) must not reach sqrtf */
    *gl = att * sqrtf((1 - pan) / 2) * 1.41421356f; *gr = att * sqrtf((1 + pan) / 2) * 1.41421356f;
}
static void mix(int16_t *out, int frames, void *user) {
    (void)user; float *acc = calloc((size_t)frames * 2, sizeof *acc);
    lock();
    float master = gain_db(master_vol);
    for (Buf *b = bufs; b; b = b->next) {
        if (!b->playing || b->primary || !b->data || !b->data->size || !b->bits) continue;
        uint32_t fb = frame_bytes(b), nfr = b->data->size / fb; if (!nfr) continue;
        double step = (double)(b->freq ? b->freq : b->rate) / (double)out_rate;
        float g = gain_db(b->volume) * master, gl = g, gr = g, l3, r3;
        if (b->pan < 0) gr *= gain_db(b->pan); else if (b->pan > 0) gl *= gain_db(-b->pan);
        gains_3d(b, &l3, &r3); gl *= l3; gr *= r3;
        const uint8_t *mem = GP(b->data->mem);
        uint32_t start_byte = (uint32_t)b->pos * fb; int stopped = 0, wrapped = 0;
        for (int i = 0; i < frames; i++) {
            uint32_t f0 = (uint32_t)b->pos; float t = (float)(b->pos - f0), l0, r0, l1, r1;
            sample_at(b, mem, nfr, f0, &l0, &r0);
            if (f0 + 1 < nfr || b->looping) sample_at(b, mem, nfr, f0 + 1, &l1, &r1); else { l1 = l0; r1 = r0; }
            acc[2 * i] += (l0 + (l1 - l0) * t) * gl; acc[2 * i + 1] += (r0 + (r1 - r0) * t) * gr;
            b->pos += step;
            if (b->pos >= nfr) { if (b->looping) { b->pos -= nfr * floor(b->pos / nfr); wrapped = 1; } else { b->pos = 0; b->playing = 0; stopped = 1; break; } }
        }
        uint32_t end_byte = (uint32_t)b->pos * fb;
        if (b->nnotify) { if (stopped) { notify_range(b, start_byte, b->data->size, 0); notify_stop(b); } else notify_range(b, start_byte, end_byte, wrapped || end_byte < start_byte); }
    }
    unlock();
    for (int i = 0; i < frames * 2; i++) { float v = acc[i] * 32767.0f; out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    free(acc);
}
static void timer_mixer(void *arg) {           /* no audio device: keep time with a thread */
    (void)arg; int16_t buf[2 * 441]; uint64_t next = plat_time_ns();
    for (;;) { mix(buf, 441, NULL); next += 10000000u; uint64_t now = plat_time_ns(); if (next > now) plat_sleep_ns(next - now); else next = now; }
}
static void start_output(void) {
    if (started) return; started = 1;
    out_rate = plat_audio_open(OUT_RATE, mix, NULL);
    if (!out_rate) { out_rate = OUT_RATE; plat_thread_start(timer_mixer, NULL, "audio (no device)"); port_log("DirectSound: no audio device; sound is mixed and discarded"); }
    else port_log("DirectSound: mixing to %d Hz stereo", out_rate);
}

/* ---------------------------------------------------------------- IDirectSound */
static void set_format(Buf *b, uint32_t wfx) {
    if (!wfx) { b->tag = 1; b->channels = 2; b->bits = 16; b->rate = 22050; b->align = 4; b->avg = 88200; return; }
    b->tag = (uint16_t)RD16(wfx); b->channels = (uint16_t)RD16(wfx + 2); b->rate = RD32(wfx + 4); b->avg = RD32(wfx + 8); b->align = (uint16_t)RD16(wfx + 12); b->bits = (uint16_t)RD16(wfx + 14);
    if (b->tag != 1) port_warn("DirectSound: format tag %u is not PCM; the buffer plays silence", b->tag);
    if (b->tag != 1 || (b->bits != 8 && b->bits != 16) || !b->channels) b->bits = 0;
}
static ComObj *new_buffer(Buf *b) {
    ComObj *o = com_create(&c_dsb, b); b->obj = o;
    lock(); b->next = bufs; bufs = b; unlock(); return o;
}
static void ds_createbuffer(CPU *c, ComObj *s) {
    (void)s; uint32_t d = A(1); if (!d || !A(2)) { RET(DSERR_INVALIDPARAM); return; }
    start_output();
    Buf *b = calloc(1, sizeof *b); b->flags = RD32(d + 4); b->primary = (b->flags & 1) != 0; b->min_d = 1; b->max_d = 1e9f; b->cone_in = b->cone_out = 360; b->cone_dir.z = 1;
    if (b->primary) set_format(b, 0);
    else {
        uint32_t size = RD32(d + 8); if (size < 4 || size > 0x0FFFFFFF) { free(b); RET(DSERR_INVALIDPARAM); return; }
        set_format(b, RD32(d + 16)); b->data = calloc(1, sizeof *b->data); b->data->refs = 1; b->data->size = size; b->data->mem = g_alloc(size);
        if (!b->data->mem) { free(b->data); free(b); RET(0x8007000Eu); return; }
    }
    WR32(A(2), new_buffer(b)->guest); RET(0);
}
static void ds_duplicate(CPU *c, ComObj *s) {
    (void)s; ComObj *src = com_get_as(A(1), &c_dsb); if (!src || B(src)->primary) { RET(DSERR_INVALIDPARAM); return; }
    Buf *o = B(src), *b = calloc(1, sizeof *b); lock(); *b = *o; b->next = NULL; b->playing = 0; b->pos = 0; b->nnotify = 0; b->notify = NULL; b->data->refs++; unlock();
    WR32(A(2), new_buffer(b)->guest); RET(0);
}
static void ds_caps(CPU *c, ComObj *s) {
    (void)s; uint32_t p = A(1), sz = RD32(p); if (sz < 96) { RET(DSERR_INVALIDPARAM); return; } memset(GP(p) + 4, 0, sz - 4);
    WR32(p + 4, 0x0F5F); WR32(p + 8, 100); WR32(p + 12, 200000); WR32(p + 16, 1); WR32(p + 20, 64); WR32(p + 24, 64); WR32(p + 28, 64); WR32(p + 32, 64); WR32(p + 36, 64); WR32(p + 40, 64); RET(0);
}
static void ds_speaker(CPU *c, ComObj *s) { (void)s; OUTP(1, 4); RET(0); }        /* DSSPEAKER_STEREO */
static void ds_destroy(ComObj *o) { (void)o; }
static const ComMethod ds_m[] = { { "CreateSoundBuffer", ds_createbuffer }, { "DuplicateSoundBuffer", ds_duplicate }, { "GetCaps", ds_caps }, { "GetSpeakerConfig", ds_speaker }, { 0, 0 } };
static const uint8_t iids_ds[][16] = { IID_IDirectSound };
ComClass c_ds = { "IDirectSound", IFACE_IDirectSound, iids_ds, 1, ds_m, DS_OK, ds_destroy };

/* ---------------------------------------------------------------- IDirectSoundBuffer */
static void dsb_destroy(ComObj *o) {
    Buf *b = o->data; lock();
    for (Buf **pp = &bufs; *pp; pp = &(*pp)->next) if (*pp == b) { *pp = b->next; break; }
    unlock();
    if (b->data && !--b->data->refs) { g_free(b->data->mem); free(b->data); }
    drop_notify(b); free(b);
}
static void dsb_caps(CPU *c, ComObj *s) { Buf *b = B(s); uint32_t p = A(1); if (RD32(p) < 20) { RET(DSERR_INVALIDPARAM); return; } WR32(p + 4, b->flags | 0x8); WR32(p + 8, b->data ? b->data->size : 4096); WR32(p + 12, 0); WR32(p + 16, 0); RET(0); }
static uint32_t play_byte(Buf *b) { return b->data ? ((uint32_t)b->pos * frame_bytes(b)) % b->data->size : 0; }
static void dsb_getpos(CPU *c, ComObj *s) {
    Buf *b = B(s); lock(); uint32_t p = play_byte(b), size = b->data ? b->data->size : 0;
    uint32_t ahead = b->playing ? (b->avg ? b->avg : 88200) * 15 / 1000 : 0; ahead -= ahead % (frame_bytes(b) ? frame_bytes(b) : 1);
    unlock(); OUTP(1, p); OUTP(2, size ? (p + ahead) % size : 0); RET(0);
}
static uint32_t wfx_size(void) { return 18; }
static void dsb_getformat(CPU *c, ComObj *s) {
    Buf *b = B(s); uint32_t p = A(1), cap = A(2), n = wfx_size();
    uint8_t w[18]; uint16_t v16; uint32_t v32;
    v16 = b->tag ? b->tag : 1; memcpy(w, &v16, 2); v16 = b->channels; memcpy(w + 2, &v16, 2); v32 = b->rate; memcpy(w + 4, &v32, 4);
    v32 = b->avg; memcpy(w + 8, &v32, 4); v16 = b->align; memcpy(w + 12, &v16, 2); v16 = b->bits ? b->bits : 16; memcpy(w + 14, &v16, 2); v16 = 0; memcpy(w + 16, &v16, 2);
    if (p && cap) memcpy(GP(p), w, cap < n ? cap : n);
    OUTP(3, n); RET(0);
}
static void dsb_setformat(CPU *c, ComObj *s) { Buf *b = B(s); if (!A(1)) { RET(DSERR_INVALIDPARAM); return; } lock(); set_format(b, A(1)); unlock(); RET(0); }
static void dsb_getvol(CPU *c, ComObj *s) { OUTP(1, (uint32_t)(B(s)->primary ? master_vol : B(s)->volume)); RET(0); }
static void dsb_setvol(CPU *c, ComObj *s) { Buf *b = B(s); int32_t v = (int32_t)A(1); if (v > 0 || v < -10000) { RET(DSERR_INVALIDPARAM); return; } lock(); if (b->primary) master_vol = v; else b->volume = v; unlock(); RET(0); }
static void dsb_getpan(CPU *c, ComObj *s) { OUTP(1, (uint32_t)B(s)->pan); RET(0); }
static void dsb_setpan(CPU *c, ComObj *s) { int32_t v = (int32_t)A(1); if (v > 10000 || v < -10000) { RET(DSERR_INVALIDPARAM); return; } lock(); B(s)->pan = v; unlock(); RET(0); }
static void dsb_getfreq(CPU *c, ComObj *s) { Buf *b = B(s); OUTP(1, b->freq ? b->freq : b->rate); RET(0); }
static void dsb_setfreq(CPU *c, ComObj *s) { uint32_t f = A(1); if (f && (f < 100 || f > 200000)) { RET(DSERR_INVALIDPARAM); return; } lock(); B(s)->freq = f; unlock(); RET(0); }
static void dsb_status(CPU *c, ComObj *s) { Buf *b = B(s); lock(); uint32_t st = b->playing ? 1u | (b->looping ? 4u : 0) : 0; unlock(); OUTP(1, st); RET(0); }
static void dsb_lock(CPU *c, ComObj *s) {
    Buf *b = B(s); uint32_t off = A(1), n = A(2), flags = A(7);
    if (b->primary || !b->data) { RET(0x88780064u); return; }      /* DSERR_PRIOLEVELNEEDED-ish: no access to the primary buffer */
    uint32_t size = b->data->size;
    if (flags & 1) { lock(); uint32_t p = play_byte(b); unlock(); off = (p + (b->playing ? (b->avg ? b->avg : 88200) * 15 / 1000 : 0)) % size; }
    if (flags & 2) { off = 0; n = size; }
    if (off >= size || n > size || !n) { RET(DSERR_INVALIDPARAM); return; }
    uint32_t c1 = n, c2 = 0; if (off + n > size) { c1 = size - off; c2 = n - c1; }
    OUTP(3, b->data->mem + off); OUTP(4, c1); OUTP(5, c2 ? b->data->mem : 0); OUTP(6, c2); RET(0);
}
static void dsb_unlock(CPU *c, ComObj *s) { (void)s; RET(0); }
static void dsb_play(CPU *c, ComObj *s) { Buf *b = B(s); lock(); b->playing = 1; b->looping = (A(3) & 1) != 0; unlock(); RET(0); }
static void dsb_setpos(CPU *c, ComObj *s) { Buf *b = B(s); if (!b->data || A(1) >= b->data->size || !frame_bytes(b)) { RET(DSERR_INVALIDPARAM); return; } lock(); b->pos = (double)(A(1) / frame_bytes(b)); unlock(); RET(0); }
static void dsb_stop(CPU *c, ComObj *s) { Buf *b = B(s); lock(); int was = b->playing; b->playing = 0; if (was && b->nnotify) notify_stop(b); unlock(); RET(0); }
static void dsb_restore(CPU *c, ComObj *s) { (void)s; RET(0); }
static ComObj *aggregate(ComObj *main, ComClass *cls) { ComObj *o = com_create(cls, NULL); o->extra_iface_of = main; com_addref(main); return o; }
static void agg_destroy(ComObj *o) { if (o->extra_iface_of) com_release(o->extra_iface_of); }
static void dsb_qi(CPU *c, ComObj *s) {
    Buf *b = B(s); uint32_t riid = A(1), out = A(2); ComObj *r = NULL;
    static const uint8_t unk[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xc0, 0, 0, 0, 0, 0, 0, 0x46 }, i_dsb[16] = IID_IDirectSoundBuffer, i_3db[16] = IID_IDirectSound3DBuffer,
                         i_3dl[16] = IID_IDirectSound3DListener, i_not[16] = IID_IDirectSoundNotify, i_ksp[16] = IID_IKsPropertySet;
    if (com_iid_eq(riid, unk) || com_iid_eq(riid, i_dsb)) { com_addref(s); r = s; }
    else if (com_iid_eq(riid, i_3db) && !b->primary && (b->flags & 0x10)) r = aggregate(s, &c_ds3b);
    else if (com_iid_eq(riid, i_3dl) && b->primary) r = aggregate(s, &c_ds3l);
    else if (com_iid_eq(riid, i_not) && !b->primary) r = aggregate(s, &c_notify);
    else if (com_iid_eq(riid, i_ksp)) r = aggregate(s, &c_ksp);
    if (out) WR32(out, r ? r->guest : 0); RET(r ? 0 : E_NOINTERFACE_);
}
static const ComMethod dsb_m[] = { { "QueryInterface", dsb_qi }, { "GetCaps", dsb_caps }, { "GetCurrentPosition", dsb_getpos }, { "GetFormat", dsb_getformat },
    { "SetFormat", dsb_setformat }, { "GetVolume", dsb_getvol }, { "SetVolume", dsb_setvol }, { "GetPan", dsb_getpan }, { "SetPan", dsb_setpan },
    { "GetFrequency", dsb_getfreq }, { "SetFrequency", dsb_setfreq }, { "GetStatus", dsb_status }, { "Lock", dsb_lock }, { "Unlock", dsb_unlock },
    { "Play", dsb_play }, { "SetCurrentPosition", dsb_setpos }, { "Stop", dsb_stop }, { "Restore", dsb_restore }, { 0, 0 } };
ComClass c_dsb = { "IDirectSoundBuffer", IFACE_IDirectSoundBuffer, NULL, 0, dsb_m, DS_OK, dsb_destroy };

/* ---------------------------------------------------------------- IDirectSound3DBuffer */
static V3 v3(CPU *c, int i) { V3 v; uint32_t a = A(i), b2 = A(i + 1), d = A(i + 2); memcpy(&v.x, &a, 4); memcpy(&v.y, &b2, 4); memcpy(&v.z, &d, 4); return v; }
static void put_v3(uint32_t p, V3 v) { memcpy(GP(p), &v.x, 4); memcpy(GP(p + 4), &v.y, 4); memcpy(GP(p + 8), &v.z, 4); }
static V3 get_v3(uint32_t p) { V3 v; memcpy(&v.x, GP(p), 4); memcpy(&v.y, GP(p + 4), 4); memcpy(&v.z, GP(p + 8), 4); return v; }
static float argf(CPU *c, int i) { uint32_t u = A(i); float f; memcpy(&f, &u, 4); return f; }
static void putf(uint32_t p, float f) { memcpy(GP(p), &f, 4); }
static void b3_getall(CPU *c, ComObj *s) {
    Buf *b = B(s); uint32_t p = A(1); lock();
    put_v3(p + 4, b->p3); put_v3(p + 16, b->v3); WR32(p + 28, b->cone_in); WR32(p + 32, b->cone_out); put_v3(p + 36, b->cone_dir); WR32(p + 48, (uint32_t)b->cone_vol);
    putf(p + 52, b->min_d); putf(p + 56, b->max_d); WR32(p + 60, b->mode3d); unlock(); RET(0);
}
static void b3_setall(CPU *c, ComObj *s) {
    Buf *b = B(s); uint32_t p = A(1); lock();
    b->p3 = get_v3(p + 4); b->v3 = get_v3(p + 16); b->cone_in = RD32(p + 28); b->cone_out = RD32(p + 32); b->cone_dir = get_v3(p + 36); b->cone_vol = (int32_t)RD32(p + 48);
    memcpy(&b->min_d, GP(p + 52), 4); memcpy(&b->max_d, GP(p + 56), 4); b->mode3d = RD32(p + 60); unlock(); RET(0);
}
static void b3_getpos(CPU *c, ComObj *s) { put_v3(A(1), B(s)->p3); RET(0); }
static void b3_setpos(CPU *c, ComObj *s) { lock(); B(s)->p3 = v3(c, 1); unlock(); RET(0); }
static void b3_getvel(CPU *c, ComObj *s) { put_v3(A(1), B(s)->v3); RET(0); }
static void b3_setvel(CPU *c, ComObj *s) { lock(); B(s)->v3 = v3(c, 1); unlock(); RET(0); }
static void b3_getmin(CPU *c, ComObj *s) { putf(A(1), B(s)->min_d); RET(0); }
static void b3_setmin(CPU *c, ComObj *s) { lock(); B(s)->min_d = argf(c, 1); unlock(); RET(0); }
static void b3_getmax(CPU *c, ComObj *s) { putf(A(1), B(s)->max_d); RET(0); }
static void b3_setmax(CPU *c, ComObj *s) { lock(); B(s)->max_d = argf(c, 1); unlock(); RET(0); }
static void b3_getmode(CPU *c, ComObj *s) { OUTP(1, B(s)->mode3d); RET(0); }
static void b3_setmode(CPU *c, ComObj *s) { lock(); B(s)->mode3d = A(1); unlock(); RET(0); }
static void b3_getcone(CPU *c, ComObj *s) { OUTP(1, B(s)->cone_in); OUTP(2, B(s)->cone_out); RET(0); }
static void b3_setcone(CPU *c, ComObj *s) { B(s)->cone_in = A(1); B(s)->cone_out = A(2); RET(0); }
static void b3_getconedir(CPU *c, ComObj *s) { put_v3(A(1), B(s)->cone_dir); RET(0); }
static void b3_setconedir(CPU *c, ComObj *s) { B(s)->cone_dir = v3(c, 1); RET(0); }
static void b3_getconevol(CPU *c, ComObj *s) { OUTP(1, (uint32_t)B(s)->cone_vol); RET(0); }
static void b3_setconevol(CPU *c, ComObj *s) { B(s)->cone_vol = (int32_t)A(1); RET(0); }
static const ComMethod ds3b_m[] = { { "GetAllParameters", b3_getall }, { "SetAllParameters", b3_setall }, { "GetPosition", b3_getpos }, { "SetPosition", b3_setpos },
    { "GetVelocity", b3_getvel }, { "SetVelocity", b3_setvel }, { "GetMinDistance", b3_getmin }, { "SetMinDistance", b3_setmin }, { "GetMaxDistance", b3_getmax },
    { "SetMaxDistance", b3_setmax }, { "GetMode", b3_getmode }, { "SetMode", b3_setmode }, { "GetConeAngles", b3_getcone }, { "SetConeAngles", b3_setcone },
    { "GetConeOrientation", b3_getconedir }, { "SetConeOrientation", b3_setconedir }, { "GetConeOutsideVolume", b3_getconevol }, { "SetConeOutsideVolume", b3_setconevol }, { 0, 0 } };
static const uint8_t iids_3db[][16] = { IID_IDirectSound3DBuffer };
ComClass c_ds3b = { "IDirectSound3DBuffer", IFACE_IDirectSound3DBuffer, iids_3db, 1, ds3b_m, DS_OK, agg_destroy };

/* ---------------------------------------------------------------- IDirectSound3DListener */
static void l_getall(CPU *c, ComObj *s) { (void)s; uint32_t p = A(1); lock(); put_v3(p + 4, lis.pos); put_v3(p + 16, lis.vel); put_v3(p + 28, lis.front); put_v3(p + 40, lis.top); putf(p + 52, lis.dist_f); putf(p + 56, lis.rolloff); putf(p + 60, lis.doppler); unlock(); RET(0); }
static void l_setall(CPU *c, ComObj *s) { (void)s; uint32_t p = A(1); lock(); lis.pos = get_v3(p + 4); lis.vel = get_v3(p + 16); lis.front = get_v3(p + 28); lis.top = get_v3(p + 40); memcpy(&lis.dist_f, GP(p + 52), 4); memcpy(&lis.rolloff, GP(p + 56), 4); memcpy(&lis.doppler, GP(p + 60), 4); unlock(); RET(0); }
static void l_getpos(CPU *c, ComObj *s) { (void)s; put_v3(A(1), lis.pos); RET(0); }
static void l_setpos(CPU *c, ComObj *s) { (void)s; lock(); lis.pos = v3(c, 1); unlock(); RET(0); }
static void l_getvel(CPU *c, ComObj *s) { (void)s; put_v3(A(1), lis.vel); RET(0); }
static void l_setvel(CPU *c, ComObj *s) { (void)s; lock(); lis.vel = v3(c, 1); unlock(); RET(0); }
static void l_getorient(CPU *c, ComObj *s) { (void)s; put_v3(A(1), lis.front); put_v3(A(2), lis.top); RET(0); }
static void l_setorient(CPU *c, ComObj *s) { (void)s; lock(); lis.front = v3(c, 1); lis.top = v3(c, 4); unlock(); RET(0); }
static void l_getdist(CPU *c, ComObj *s) { (void)s; putf(A(1), lis.dist_f); RET(0); }
static void l_setdist(CPU *c, ComObj *s) { (void)s; lock(); lis.dist_f = argf(c, 1); unlock(); RET(0); }
static void l_getroll(CPU *c, ComObj *s) { (void)s; putf(A(1), lis.rolloff); RET(0); }
static void l_setroll(CPU *c, ComObj *s) { (void)s; lock(); lis.rolloff = argf(c, 1); unlock(); RET(0); }
static void l_getdop(CPU *c, ComObj *s) { (void)s; putf(A(1), lis.doppler); RET(0); }
static void l_setdop(CPU *c, ComObj *s) { (void)s; lis.doppler = argf(c, 1); RET(0); }
static const ComMethod ds3l_m[] = { { "GetAllParameters", l_getall }, { "SetAllParameters", l_setall }, { "GetPosition", l_getpos }, { "SetPosition", l_setpos },
    { "GetVelocity", l_getvel }, { "SetVelocity", l_setvel }, { "GetOrientation", l_getorient }, { "SetOrientation", l_setorient }, { "GetDistanceFactor", l_getdist },
    { "SetDistanceFactor", l_setdist }, { "GetRolloffFactor", l_getroll }, { "SetRolloffFactor", l_setroll }, { "GetDopplerFactor", l_getdop }, { "SetDopplerFactor", l_setdop }, { 0, 0 } };
static const uint8_t iids_3dl[][16] = { IID_IDirectSound3DListener };
ComClass c_ds3l = { "IDirectSound3DListener", IFACE_IDirectSound3DListener, iids_3dl, 1, ds3l_m, DS_OK, agg_destroy };

/* ---------------------------------------------------------------- IDirectSoundNotify and IKsPropertySet (no EAX) */
static void n_setpos(CPU *c, ComObj *s) {
    Buf *b = B(s); uint32_t n = A(1), p = A(2); if (n > 1024 || (n && !p)) { RET(DSERR_INVALIDPARAM); return; }
    lock(); if (b->playing) { unlock(); RET(0x88780032u); return; }       /* DSERR_INVALIDCALL: not while playing */
    drop_notify(b); b->notify = n ? calloc(n, sizeof *b->notify) : NULL; b->nnotify = n;
    for (uint32_t i = 0; i < n; i++) { b->notify[i].off = RD32(p + 8 * i); b->notify[i].ev = handle_get(RD32(p + 8 * i + 4), K_EVENT); if (b->notify[i].ev) kobj_ref(b->notify[i].ev); }
    unlock(); RET(0);
}
static const ComMethod notify_m[] = { { "SetNotificationPositions", n_setpos }, { 0, 0 } };
static const uint8_t iids_not[][16] = { IID_IDirectSoundNotify };
ComClass c_notify = { "IDirectSoundNotify", IFACE_IDirectSoundNotify, iids_not, 1, notify_m, DS_OK, agg_destroy };
static void ksp_query(CPU *c, ComObj *s) { (void)s; OUTP(3, 0); RET(0); }
static const ComMethod ksp_m[] = { { "QuerySupport", ksp_query }, { 0, 0 } };
static const uint8_t iids_ksp[][16] = { IID_IKsPropertySet };
ComClass c_ksp = { "IKsPropertySet", IFACE_IKsPropertySet, iids_ksp, 1, ksp_m, 0x80070490u, agg_destroy };      /* E_PROP_ID_UNSUPPORTED */

/* ---------------------------------------------------------------- dsound.dll exports */
static void ensure(void) { if (!mx) mx = plat_mutex_new(); }
SHIM(DirectSoundCreate) { ensure(); if (!A(1)) { RET(DSERR_INVALIDPARAM); return; } WR32(A(1), com_create(&c_ds, NULL)->guest); RET(0); }
SHIM(DirectSoundEnumerateA) { ensure(); g_call(c, A(0), 4, 0u, g_str("Primary Sound Driver"), g_str(""), A(1)); RET(0); }
const ShimDef dsound_shims[] = {
    { "DirectSoundCreate", sh_DirectSoundCreate, STD(3) }, { "DirectSoundEnumerateA", sh_DirectSoundEnumerateA, STD(2) },
    { 0, 0, 0 }
};
