/* plat_vita.c: the PS Vita backend (window, input, audio, start-up). Rendering is the shared OpenGL ES 2.0 renderer
 * (backends/sdl2/gl_render.c) on vitaGL, which compiles its generated GLSL at run time (needs libshacccg.suprx).
 *
 * Files on the memory card, all in ux0:data/ss2/ (VITA_GAME_DIR):
 *   SS2.exe and the rest of the game, exactly as on the PC the build was generated from
 *   ss2port.txt  optional: options for the host, one or more per line, e.g. "--guest-space 208 --guest-backed 144 --verbose"
 *                plus the Vita-only options below
 *   ss2port.log  everything the host and the renderer log (rewritten on every start)
 *
 * Vita-only options (in ss2port.txt):
 *   --swap-sticks        move with the right stick, look with the left
 *   --look-speed N       right-stick look speed, default 100 (percent)
 *
 * Controls (keys meant to match SS2's usual binds; check them against your bind file and change the table below):
 *   left stick  W A S D           right stick  mouse look / cursor
 *   R           left mouse (fire) L            right mouse (use / frob)
 *   Cross       Space (jump)      Circle       C (crouch)
 *   Square      R (reload)        Triangle     Tab (shoot / interface mode)
 *   D-pad up/down  mouse wheel    D-pad left/right  Q / E (lean)
 *   Start       Esc (menu)        Select       I (inventory)
 *   front touch: cursor position and left click (for inventory, PDA and hacking screens) */
#include <stdio.h>
#include <psp2/kernel/cpu.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>
#include <psp2/ctrl.h>
#include <psp2/touch.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>
#include "plat.h"
#include "gl_render.h"

#ifndef VITA_GAME_DIR
#define VITA_GAME_DIR "ux0:data/ss2"
#endif
#ifndef VITA_HEAP_MB                 /* newlib heap for the host (file buffers, tables, the renderer's CPU-side data) */
#define VITA_HEAP_MB 40
#endif
#ifndef VITA_GUEST_SPACE_MB_STR       /* guest address space, and how much of it is real RAM (see vm_alloc: the game's 64 MB startup pool is mostly never touched) */
#define VITA_GUEST_SPACE_MB_STR "208"
#endif
#ifndef VITA_GUEST_BACKED_MB_STR
#define VITA_GUEST_BACKED_MB_STR "128"
#endif
#ifndef VITA_VGL_RAM_MB             /* main-RAM pool of vitaGL (vertex data etc.) */
#define VITA_VGL_RAM_MB 6
#endif
#ifndef VITA_RAM_THRESHOLD_MB        /* RAM vitaGL leaves free when it sizes its pools (thread stacks are allocated later) */
#define VITA_RAM_THRESHOLD_MB 32
#endif
#define SCREEN_W 960
#define SCREEN_H 544

unsigned int _newlib_heap_size_user = VITA_HEAP_MB * 1024 * 1024;
void vita_log_free_memory(const char *when);
static void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void vlog(const char *fmt, ...) { char b[300]; int n = snprintf(b, sizeof b, "[vita] "); va_list ap; va_start(ap, fmt); vsnprintf(b + n, sizeof b - (size_t)n, fmt, ap); va_end(ap); plat_log_write(PLAT_LOG_INFO, b); }

/* ---------------------------------------------------------------- display */
static int surf_w = 640, surf_h = 480, gl_ready, rel_mode;
static void letterbox(int *x, int *y, int *w, int *h) {
    double s = (double)SCREEN_W / surf_w < (double)SCREEN_H / surf_h ? (double)SCREEN_W / surf_w : (double)SCREEN_H / surf_h;
    *w = (int)(surf_w * s + 0.5); *h = (int)(surf_h * s + 0.5); *x = (SCREEN_W - *w) / 2; *y = (SCREEN_H - *h) / 2;
}
void gl_present_rect(int *x, int *y, int *w, int *h, int *ww, int *wh) { letterbox(x, y, w, h); *ww = SCREEN_W; *wh = SCREEN_H; }
/* every 5 s: frame rate, and how the time splits between the file layer, presenting the picture and everything else (the game's CPU work) */
extern uint64_t vp_fs_ns, vp_fs_open, vp_fs_stat, vp_fs_read_calls, vp_fs_sys_reads, vp_fs_bytes;
extern volatile uint64_t vp_fs_activity;
uint64_t gl_prof_ns, gl_prof_draws;
static uint64_t swap_ns; static unsigned frames;
void vita_log_guest_highwater(const char *when);
void vita_log_inflight(void);
void vita_log_readstats(void);
void vita_profile_tick(uint64_t now) {
    static uint64_t t0, last_swap, last_fs, last_gl, d0, o0, st0, rc0, sr0, b0; static unsigned f0;
    if (!t0) { t0 = now; return; }
    if (now - t0 < 5000000000ull) return;
    double dt = (double)(now - t0) / 1e9;
    double gl = (double)(gl_prof_ns - last_gl), fs = (double)(vp_fs_ns - last_fs), sw = (double)(swap_ns - last_swap); unsigned nf = frames - f0;
    vlog("profile: %.1f fps | render %.0f%% (swap %.0f%%, %llu draws/frame) | files %.0f%% (%llu opens, %llu stats, %llu reads -> %llu system reads, %.1f MB) | game cpu %.0f%%", nf / dt,
         100.0 * gl / (dt * 1e9), 100.0 * sw / (dt * 1e9), (unsigned long long)(nf ? (gl_prof_draws - d0) / nf : 0),
         100.0 * fs / (dt * 1e9), (unsigned long long)(vp_fs_open - o0), (unsigned long long)(vp_fs_stat - st0),
         (unsigned long long)(vp_fs_read_calls - rc0), (unsigned long long)(vp_fs_sys_reads - sr0), (double)(vp_fs_bytes - b0) / 1048576.0,
         100.0 - 100.0 * (gl + fs) / (dt * 1e9));
    { static int n; if (++n % 3 == 0) { vita_log_guest_highwater("now"); vita_log_readstats(); } }
    t0 = now; last_swap = swap_ns; last_fs = vp_fs_ns; last_gl = gl_prof_ns; d0 = gl_prof_draws; o0 = vp_fs_open; st0 = vp_fs_stat; rc0 = vp_fs_read_calls; sr0 = vp_fs_sys_reads; b0 = vp_fs_bytes; f0 = frames;
}
uint64_t vita_now_ns(void);
const char *volatile vita_where; volatile uint64_t vita_where_t0;
void gl_swap(void) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    const char *outer = vita_where; vita_where = "vglSwapBuffers";
    vglSwapBuffers(GL_FALSE);
    vita_where = outer;
    clock_gettime(CLOCK_MONOTONIC, &b); swap_ns += (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000u + (uint64_t)(b.tv_nsec - a.tv_nsec); frames++;
    vita_profile_tick((uint64_t)b.tv_sec * 1000000000u + (uint64_t)b.tv_nsec);
}
int plat_video_open(int w, int h, int fullscreen, const char *title) {
    (void)fullscreen; (void)title;
    surf_w = w > 0 ? w : 640; surf_h = h > 0 ? h : 480;
    if (!gl_ready) {
        vita_log_free_memory("before vitaGL");
        mkdir(VITA_GAME_DIR "/shader_cache", 0777); vglSetShaderCachePath(VITA_GAME_DIR "/shader_cache");   /* used if vitaGL was built with HAVE_SHADER_CACHE=1 */
        /* explicit pools, not "whatever RAM is left": the guest block has taken nearly all of it. Textures and buffers live in the 112 MB of video memory */
        static int vgl_tried;
        if (!vgl_tried) {
            vgl_tried = 1;
            /* the result is NOT a success flag: GL_TRUE only means the display size was clamped to what the Vita can show */
            int clamped = vglInitWithCustomSizes(0, SCREEN_W, SCREEN_H, VITA_VGL_RAM_MB * 1024 * 1024, 96 * 1024 * 1024, 0, 0, SCE_GXM_MULTISAMPLE_NONE);
            vlog("vitaGL started (RAM pool %d MB%s)", VITA_VGL_RAM_MB, clamped ? ", display size was clamped" : "");
        }       /* started once only: a second plat_video_open reuses it */
        vita_log_free_memory("after vitaGL");
        if (gl_init(vglGetProcAddress, 1)) return -1;
        gl_ready = 1;
    }
    vlog("game picture %dx%d, shown at %dx%d", surf_w, surf_h, SCREEN_W, SCREEN_H);
    gl_resize_backbuffer(surf_w, surf_h);
    return 0;
}
void plat_video_close(void) { if (gl_ready) gl_shutdown(); }    /* vitaGL stays up until the process exits; a reopen reuses it */
void plat_video_caps(PlatVideoCaps *c) { gl_caps(c); }
void plat_video_window_size(int *w, int *h) { *w = surf_w; *h = surf_h; }
void plat_video_display_size(int *w, int *h) { *w = SCREEN_W; *h = SCREEN_H; }
void plat_video_set_title(const char *t) { (void)t; }

/* ---------------------------------------------------------------- input */
enum { DIK_ESC = 0x01, DIK_TAB = 0x0F, DIK_Q = 0x10, DIK_W = 0x11, DIK_E = 0x12, DIK_R = 0x13, DIK_I = 0x17, DIK_A = 0x1E, DIK_S = 0x1F,
       DIK_D = 0x20, DIK_C = 0x2E, DIK_SPACE = 0x39 };
enum { MB_LEFT = 0, MB_RIGHT = 1 };
typedef struct { unsigned button; int dik; int mouse; } Bind;      /* mouse: -1 = key, else a mouse button */
static const Bind binds[] = {
    { SCE_CTRL_CROSS, DIK_SPACE, -1 }, { SCE_CTRL_CIRCLE, DIK_C, -1 }, { SCE_CTRL_SQUARE, DIK_R, -1 }, { SCE_CTRL_TRIANGLE, DIK_TAB, -1 },
    { SCE_CTRL_START, DIK_ESC, -1 }, { SCE_CTRL_SELECT, DIK_I, -1 }, { SCE_CTRL_LEFT, DIK_Q, -1 }, { SCE_CTRL_RIGHT, DIK_E, -1 },
    { SCE_CTRL_RTRIGGER, 0, MB_LEFT }, { SCE_CTRL_LTRIGGER, 0, MB_RIGHT },
};
static int swap_sticks; static float look_speed = 1.0f;
static unsigned prev_buttons; static int stick_keys[4];            /* W A S D currently held because of the stick */
static float cur_x, cur_y, frac_x, frac_y; static uint64_t last_poll;
static int touching;

void plat_video_mouse_mode(int relative, int visible) { (void)visible; rel_mode = relative; }
void plat_video_warp_mouse(int x, int y) { cur_x = (float)x; cur_y = (float)y; }

static void key(void (*sink)(const PlatEvent *, void *), void *user, int dik, int down) {
    PlatEvent p; memset(&p, 0, sizeof p); p.type = PLAT_EV_KEY; p.key = dik; p.down = down; sink(&p, user);
}
static void button(void (*sink)(const PlatEvent *, void *), void *user, int b, int down) {
    PlatEvent p; memset(&p, 0, sizeof p); p.type = PLAT_EV_MOUSE_BUTTON; p.button = b; p.down = down; sink(&p, user);
}
static void move(void (*sink)(const PlatEvent *, void *), void *user, int dx, int dy) {
    PlatEvent p; memset(&p, 0, sizeof p); p.type = PLAT_EV_MOUSE_MOVE; p.dx = dx; p.dy = dy;
    if (cur_x < 0) cur_x = 0; if (cur_y < 0) cur_y = 0; if (cur_x > surf_w - 1) cur_x = (float)(surf_w - 1); if (cur_y > surf_h - 1) cur_y = (float)(surf_h - 1);
    p.x = (int)cur_x; p.y = (int)cur_y; sink(&p, user);
}
/* stick value 0..255 (128 centre) -> -1..1 with a dead zone */
static float axis(unsigned char v) { float f = ((float)v - 127.5f) / 127.5f, dz = 0.18f; if (f > -dz && f < dz) return 0; return f > 0 ? (f - dz) / (1 - dz) : (f + dz) / (1 - dz); }
void plat_video_poll(void (*sink)(const PlatEvent *, void *), void *user) {
    SceCtrlData pad; memset(&pad, 0, sizeof pad);
    if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return;
    uint64_t now = plat_time_ns(); float dt = last_poll ? (float)(now - last_poll) / 1e9f : 0; if (dt > 0.1f) dt = 0.1f; last_poll = now;

    unsigned changed = pad.buttons ^ prev_buttons;
    for (size_t i = 0; i < sizeof binds / sizeof *binds; i++) if (changed & binds[i].button) {
        int down = (pad.buttons & binds[i].button) != 0;
        if (binds[i].mouse >= 0) button(sink, user, binds[i].mouse, down); else key(sink, user, binds[i].dik, down);
    }
    if ((changed & pad.buttons) & SCE_CTRL_UP)   { PlatEvent p; memset(&p, 0, sizeof p); p.type = PLAT_EV_MOUSE_WHEEL; p.dy = 120;  sink(&p, user); }
    if ((changed & pad.buttons) & SCE_CTRL_DOWN) { PlatEvent p; memset(&p, 0, sizeof p); p.type = PLAT_EV_MOUSE_WHEEL; p.dy = -120; sink(&p, user); }
    prev_buttons = pad.buttons;

    unsigned char mx = swap_sticks ? pad.rx : pad.lx, my = swap_sticks ? pad.ry : pad.ly, lx = swap_sticks ? pad.lx : pad.rx, ly = swap_sticks ? pad.ly : pad.ry;
    /* movement stick -> W A S D, with hysteresis so a stick resting near the threshold does not chatter */
    static const int dik4[4] = { DIK_W, DIK_A, DIK_S, DIK_D };
    int want[4] = { my < 128 - 44, mx < 128 - 44, my > 127 + 44, mx > 127 + 44 };
    int keep[4] = { my < 128 - 32, mx < 128 - 32, my > 127 + 32, mx > 127 + 32 };
    for (int i = 0; i < 4; i++) { int on = stick_keys[i] ? keep[i] : want[i]; if (on != stick_keys[i]) { stick_keys[i] = on; key(sink, user, dik4[i], on); } }

    /* look stick -> mouse motion (and the cursor, for menus) */
    float ax = axis(lx), ay = axis(ly);
    if (ax || ay) {
        float speed = 900.0f * look_speed;                           /* pixels per second at full tilt; curved for fine aim */
        float fx = ax * (ax < 0 ? -ax : ax) * speed * dt + frac_x, fy = ay * (ay < 0 ? -ay : ay) * speed * dt + frac_y;
        int dx = (int)fx, dy = (int)fy; frac_x = fx - (float)dx; frac_y = fy - (float)dy;
        if (dx || dy) { cur_x += (float)dx; cur_y += (float)dy; move(sink, user, dx, dy); }
    }

    /* front touch -> absolute cursor and left button (the front panel reports 0..1919 x 0..1087) */
    SceTouchData t; memset(&t, 0, sizeof t);
    if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &t, 1) >= 0) {
        if (t.reportNum > 0) {
            int bx, by, bw, bh; letterbox(&bx, &by, &bw, &bh);
            float sx = t.report[0].x * (SCREEN_W / 1920.0f), sy = t.report[0].y * (SCREEN_H / 1088.0f);
            float nx = (sx - (float)bx) * (float)surf_w / (float)bw, ny = (sy - (float)by) * (float)surf_h / (float)bh;
            int dx = (int)(nx - cur_x), dy = (int)(ny - cur_y); cur_x = nx; cur_y = ny;
            move(sink, user, rel_mode ? 0 : dx, rel_mode ? 0 : dy);
            if (!touching) { touching = 1; button(sink, user, MB_LEFT, 1); }
        } else if (touching) { touching = 0; button(sink, user, MB_LEFT, 0); }
    }
}

/* ---------------------------------------------------------------- audio: one BGM port fed by a thread */
#define AUDIO_FRAMES 1024
static int aport = -1; static volatile int arunning, apaused;
static void (*afill)(int16_t *, int, void *); static void *auser;
static void audio_thread(void *u) {
    (void)u; static int16_t buf[2][AUDIO_FRAMES * 2]; int k = 0;
    while (arunning) {
        if (apaused) memset(buf[k], 0, sizeof buf[k]); else afill(buf[k], AUDIO_FRAMES, auser);
        sceAudioOutOutput(aport, buf[k]);                         /* blocks until the previous buffer has played */
        k ^= 1;
    }
}
int plat_audio_open(int rate, void (*fill)(int16_t *, int, void *), void *user) {
    if (getenv("SS2PORT_NOSOUND")) return 0;
    static const int ok[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000 };
    int r = 48000; for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) if (ok[i] == rate) r = rate;
    aport = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, AUDIO_FRAMES, r, SCE_AUDIO_OUT_MODE_STEREO);
    if (aport < 0) { vlog("audio port: error %08x", (unsigned)aport); aport = -1; return 0; }
    afill = fill; auser = user; arunning = 1; apaused = 0;
    if (plat_thread_start(audio_thread, NULL, "audio")) { sceAudioOutReleasePort(aport); aport = -1; arunning = 0; return 0; }
    return r;
}
void plat_audio_close(void) { if (aport >= 0) { arunning = 0; plat_sleep_ns(100000000); sceAudioOutReleasePort(aport); aport = -1; } }
void plat_audio_pause(int p) { apaused = p; }

void plat_alert(const char *title, const char *text) { vlog("ALERT %s: %s", title ? title : "", text ? text : ""); }

/* ---------------------------------------------------------------- start-up */
int port_main(int argc, char **argv);
typedef struct { int argc; char **argv; int ret; } MainArgs;
static void *game_thread(void *p) {
    sceKernelChangeThreadCpuAffinityMask(sceKernelGetThreadId(), SCE_KERNEL_CPU_MASK_USER_0);    /* a core of its own */
    MainArgs *a = p; a->ret = port_main(a->argc, a->argv); return NULL;
}

/* Watchdog: if the game neither draws a frame nor reads a file for VITA_WATCHDOG_S seconds, it has hung. The Vita writes
 * no dump for a hang, so crash on purpose: the dump then holds the game thread's registers and stack, which show where it
 * is stuck. Counted in 1-second sleeps, so time spent suspended (PS button) does not count. --no-watchdog turns it off. */
#ifndef VITA_WATCHDOG_S
#define VITA_WATCHDOG_S 90
#endif
static int watchdog_on = 1; extern int vita_readahead;
static void *watchdog(void *u) {
    (void)u; unsigned last_f = 0; uint64_t last_r = 0; int idle = 0;
    for (;;) {
        sceKernelDelayThread(1000000);
        unsigned f = *(volatile unsigned *)&frames; uint64_t r = *(volatile uint64_t *)&vp_fs_read_calls + vp_fs_activity;
        if (f != last_f || r != last_r || !f) { last_f = f; last_r = r; idle = 0; continue; }
        if (++idle == VITA_WATCHDOG_S) {
            vlog("watchdog: no frame and no file call for %d s; the game has hung. Crashing on purpose so the Vita writes a core dump (send it with this log)", VITA_WATCHDOG_S);
            vita_log_inflight(); vita_log_guest_highwater("at the hang");
            { const char *w = vita_where; if (w) vlog("renderer call in progress: %s (for %u s)", w, (unsigned)((vita_now_ns() - vita_where_t0) / 1000000000u)); else vlog("no renderer call in progress"); }
            sceKernelDelayThread(200000);
            *(volatile int *)0 = 0;
        }
    }
    return NULL;
}

/* options from ss2port.txt: whitespace-separated, "quoted" for spaces, # starts a comment line */
static int read_options(const char *path, char **out, int max) {
    FILE *f = fopen(path, "r"); if (!f) return 0;
    int n = 0; char line[512];
    while (fgets(line, sizeof line, f) && n < max) {
        char *p = line;
        while (*p && n < max) {
            while (isspace((unsigned char)*p)) p++;
            if (!*p || *p == '#') break;
            char *s; if (*p == '"') { s = ++p; while (*p && *p != '"') p++; } else { s = p; while (*p && !isspace((unsigned char)*p)) p++; }
            char c = *p; *p = 0; out[n++] = strdup(s); if (!c) break; p++;
        }
    }
    fclose(f); return n;
}
static int ends_with_exe(const char *s) { size_t l = strlen(s); return l >= 4 && !strcasecmp(s + l - 4, ".exe"); }

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    mkdir(VITA_GAME_DIR, 0777);
    if (!freopen(VITA_GAME_DIR "/ss2port.log", "w", stderr)) freopen("ux0:data/ss2port.log", "w", stderr);
    setvbuf(stderr, NULL, _IOLBF, 1024);
    scePowerSetArmClockFrequency(444); scePowerSetBusClockFrequency(222); scePowerSetGpuClockFrequency(222); scePowerSetGpuXbarClockFrequency(166);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);
    sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT, SCE_TOUCH_SAMPLING_STATE_START);
    chdir(VITA_GAME_DIR);
    vita_log_free_memory("at start");

    /* argv for the host: options from ss2port.txt (Vita-only ones removed), then the exe */
    char *opt[64]; int nopt = read_options(VITA_GAME_DIR "/ss2port.txt", opt, 64);
    static char *av[80]; int ac = 0, have_space = 0, have_exe = 0;
    av[ac++] = "ss2port";
    for (int i = 0; i < nopt; i++) {
        if (!strcmp(opt[i], "--swap-sticks")) { swap_sticks = 1; continue; }
        if (!strcmp(opt[i], "--no-watchdog")) { watchdog_on = 0; continue; }
        if (!strcmp(opt[i], "--no-readahead")) { vita_readahead = 0; continue; }
        if (!strcmp(opt[i], "--look-speed") && i + 1 < nopt) { look_speed = (float)atof(opt[++i]) / 100.0f; continue; }
        if (!strcmp(opt[i], "--guest-space") || !strcmp(opt[i], "--guest-backed")) have_space = 1;
        if (ends_with_exe(opt[i])) have_exe = 1;
        if (ac < 76) av[ac++] = opt[i];
    }
    if (!have_space) {                                   /* must come before the exe: everything after it goes to the game */
        memmove(av + 5, av + 1, (size_t)(ac - 1) * sizeof *av); av[1] = "--guest-space"; av[2] = VITA_GUEST_SPACE_MB_STR; av[3] = "--guest-backed"; av[4] = VITA_GUEST_BACKED_MB_STR; ac += 4;
    }
    if (!have_exe) av[ac++] = VITA_GAME_DIR "/SS2.exe";
    av[ac] = NULL;
    { char b[400]; int n = snprintf(b, sizeof b, "[vita] command line:"); for (int i = 1; i < ac && n < (int)sizeof b - 2; i++) n += snprintf(b + n, sizeof b - (size_t)n, " %s", av[i]); plat_log_write(PLAT_LOG_INFO, b); }

    /* the game runs on its own thread: recompiled code uses the host stack for every guest call */
    MainArgs a = { ac, av, 1 }; pthread_t t; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 8u << 20);
    if (pthread_create(&t, &at, game_thread, &a)) { vlog("could not start the game thread"); return 1; }
    if (watchdog_on) { pthread_t w; pthread_attr_t wa; pthread_attr_init(&wa); pthread_attr_setstacksize(&wa, 64u << 10); pthread_create(&w, &wa, watchdog, NULL); pthread_attr_destroy(&wa); }
    pthread_join(t, NULL);
    vlog("exit %d", a.ret);
    sceKernelExitProcess(a.ret);
    return a.ret;
}
