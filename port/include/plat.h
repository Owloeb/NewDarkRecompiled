/* plat.h: the platform interface of the portable host.
 *
 * Everything the recompiled game needs from a real machine goes through the functions declared here, and nothing else in
 * port/ uses an operating-system API. To bring the game to a new platform you implement this header (or reuse the parts
 * the existing backends already provide) and build the rest of port/ unchanged:
 *
 *   sys     memory for the guest address space, clocks, threads, locks, logging       backends/posix provides it for POSIX
 *   fs      files and directories with host paths                                   backends/posix provides it for POSIX
 *   video   one window, input events, and a small fixed-function triangle renderer    backends/null, backends/sdl2
 *   audio   one stereo output stream that pulls 16-bit samples                        backends/null, backends/sdl2
 *
 * Conventions
 * - C99. All functions are called from the thread that holds the guest lock unless noted ("any thread").
 * - Paths given to plat_fs_* are host paths already mapped from Windows paths by the core (separator '/', case resolved).
 * - Return values: 0 / non-NULL for success unless documented otherwise. No function may call back into port/ code except
 *   through the callbacks it was handed (the audio callback, the event sink).
 * - The renderer receives triangles that are already transformed to clip space, with per-vertex colours, fog and up to
 *   two texture coordinate sets, plus a compact description of the fixed-function state (PlatDrawState). The Direct3D 9
 *   front end (dx/d3d9.c) does everything Direct3D-specific: formats, FVF decoding, transforms, lighting, state blocks.
 */
#ifndef PLAT_H
#define PLAT_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================ sys */
typedef enum { PLAT_LOG_DEBUG, PLAT_LOG_INFO, PLAT_LOG_WARN, PLAT_LOG_ERROR } PlatLogLevel;
void plat_log_write(PlatLogLevel level, const char *line);            /* one line without newline; any thread */

/* Reserve `size` bytes of address space for the guest, readable and writable, zero-filled. The core asks for 4 GB on
 * 64-bit hosts (so every 32-bit guest address is backed and wild guest pointers cannot reach host memory) and for the
 * configured size elsewhere. Pages may be committed lazily. Returns NULL on failure. */
extern uint64_t g_plat_backed;   /* set before plat_mem_reserve: only this many bytes from the start need to be real memory (0: all). A backend may honour it or ignore it (then everything is backed) */
void *plat_mem_reserve(uint64_t size);
void  plat_mem_release(void *base, uint64_t size);
/* Optional hint that a guest range is no longer used (VirtualFree MEM_DECOMMIT/RELEASE); the contents must read back as
 * zero afterwards if the core touches them again. May do nothing. */
void  plat_mem_discard(void *addr, uint64_t size);

uint64_t plat_time_ns(void);                  /* monotonic clock; any thread */
int64_t  plat_wall_time_ns(void);             /* nanoseconds since 1970-01-01 UTC; any thread */
int      plat_utc_offset_minutes(void);       /* local time = UTC + this; may return 0 */
void     plat_sleep_ns(uint64_t ns);          /* any thread */

typedef struct PlatMutex PlatMutex;           /* non-recursive */
typedef struct PlatCond PlatCond;
int         plat_thread_start(void (*fn)(void *), void *arg, const char *name);   /* detached; 0 on success; any thread */
void        plat_thread_yield(void);
PlatMutex  *plat_mutex_new(void);
void        plat_mutex_free(PlatMutex *m);
void        plat_mutex_lock(PlatMutex *m);
void        plat_mutex_unlock(PlatMutex *m);
PlatCond   *plat_cond_new(void);
void        plat_cond_free(PlatCond *c);
void        plat_cond_wait(PlatCond *c, PlatMutex *m);
int         plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t timeout_ns);    /* 0 signalled (or spurious), 1 timed out */
void        plat_cond_signal(PlatCond *c);
void        plat_cond_broadcast(PlatCond *c);

/* A message for the player that must not be missed (fatal error); may just log. */
void plat_alert(const char *title, const char *text);

/* ================================================================ fs */
typedef struct PlatFile PlatFile;
enum { PLAT_READ = 1, PLAT_WRITE = 2, PLAT_CREATE = 4, PLAT_TRUNCATE = 8, PLAT_EXCLUSIVE = 16, PLAT_APPEND = 32 };
enum { PLAT_SEEK_SET, PLAT_SEEK_CUR, PLAT_SEEK_END };
enum { PLAT_OK = 0, PLAT_E_NOENT = -1, PLAT_E_EXIST = -2, PLAT_E_ACCESS = -3, PLAT_E_NOTDIR = -4, PLAT_E_ISDIR = -5, PLAT_E_NOTEMPTY = -6, PLAT_E_IO = -7 };
typedef struct { uint64_t size; int64_t mtime_ns, atime_ns, ctime_ns; int is_dir; int readonly; } PlatStat;

PlatFile *plat_fs_open(const char *path, int flags, int *err);   /* err: PLAT_E_* when NULL is returned */
void      plat_fs_close(PlatFile *f);
int64_t   plat_fs_read(PlatFile *f, void *buf, uint64_t n);       /* bytes read, 0 at end, <0 error */
int64_t   plat_fs_write(PlatFile *f, const void *buf, uint64_t n);
int64_t   plat_fs_seek(PlatFile *f, int64_t off, int whence);     /* new position or <0 */
int       plat_fs_truncate(PlatFile *f, uint64_t size);
int       plat_fs_flush(PlatFile *f);
int       plat_fs_fstat(PlatFile *f, PlatStat *st);
int       plat_fs_stat(const char *path, PlatStat *st);           /* PLAT_OK or PLAT_E_* */
int       plat_fs_mkdir(const char *path);
int       plat_fs_rmdir(const char *path);
int       plat_fs_remove(const char *path);
int       plat_fs_rename(const char *from, const char *to);
typedef struct PlatDir PlatDir;
PlatDir    *plat_fs_opendir(const char *path);
const char *plat_fs_readdir(PlatDir *d);                         /* next entry name (not "." or ".."), NULL at the end */
void        plat_fs_closedir(PlatDir *d);
/* Directory for files the game writes (saves, config, logs) when it differs from the game folder; NULL = game folder. */
const char *plat_fs_write_dir(void);

/* ================================================================ video: window and input */
typedef enum {
    PLAT_EV_NONE, PLAT_EV_QUIT, PLAT_EV_FOCUS, PLAT_EV_RESIZE,
    PLAT_EV_KEY,           /* key: scan code in DirectInput numbering (DIK_*: set-1 scan codes, extended keys + 0x80) */
    PLAT_EV_TEXT,          /* text: one Unicode code point the player typed */
    PLAT_EV_MOUSE_MOVE,    /* x, y: absolute position in the drawing surface's pixels (0..w-1, 0..h-1 of plat_video_open); dx, dy: raw relative motion */
    PLAT_EV_MOUSE_BUTTON,  /* button: 0 left, 1 right, 2 middle, 3, 4 extra */
    PLAT_EV_MOUSE_WHEEL    /* dy: +120 per notch away from the player, like WM_MOUSEWHEEL */
} PlatEventType;
typedef struct {
    PlatEventType type;
    int down;              /* KEY, MOUSE_BUTTON: pressed (1) or released (0); FOCUS: gained (1) or lost (0) */
    int key;               /* KEY */
    int button;            /* MOUSE_BUTTON */
    uint32_t text;         /* TEXT */
    int x, y, dx, dy;      /* MOUSE_MOVE, MOUSE_WHEEL, RESIZE (x, y = new client size) */
    int repeat;            /* KEY: auto-repeat */
} PlatEvent;

typedef struct {
    int max_texture_size;  /* largest texture edge */
    int dxt;               /* compressed DXT1/3/5 textures can be uploaded as they are */
    int npot;              /* non-power-of-two textures with mipmaps and wrapping */
    int render_targets;    /* textures can be rendered to */
    int max_anisotropy;
} PlatVideoCaps;

/* Opens (or reconfigures) the one game window with a drawing surface of w x h pixels. fullscreen is a request; the
 * backend may letterbox/scale. Returns 0 on success. */
int  plat_video_open(int w, int h, int fullscreen, const char *title);
void plat_video_close(void);
void plat_video_caps(PlatVideoCaps *caps);
void plat_video_window_size(int *w, int *h);                     /* size of the drawing surface (what plat_video_open was given) */
void plat_video_display_size(int *w, int *h);                    /* desktop / screen size */
/* Calls sink for every pending event. Must be called regularly by the core (it is, from the message pump and Present). */
void plat_video_poll(void (*sink)(const PlatEvent *ev, void *user), void *user);
void plat_video_set_title(const char *title);
void plat_video_mouse_mode(int relative, int visible);            /* relative: hide and capture the pointer for mouse look */
void plat_video_warp_mouse(int x, int y);

/* ================================================================ video: renderer */
typedef struct PlatTexture PlatTexture;
typedef enum { PLAT_TEX_RGBA8, PLAT_TEX_DXT1, PLAT_TEX_DXT3, PLAT_TEX_DXT5 } PlatTexFormat;   /* RGBA8: bytes R,G,B,A */
PlatTexture *plat_tex_create(int w, int h, int levels, PlatTexFormat fmt, int render_target);
void plat_tex_upload(PlatTexture *t, int level, int x, int y, int w, int h, const void *data, int pitch);
void plat_tex_destroy(PlatTexture *t);
/* reads back w x h RGBA8 pixels of a render target (NULL = the back buffer) into out; returns 0 on success */
int  plat_tex_read(PlatTexture *t, int x, int y, int w, int h, void *out, int pitch);

/* A vertex in clip space with Direct3D's conventions: y points up, z runs from 0 (near) to w (far). The front end has
 * already moved the geometry by half a pixel, so a backend that samples pixel centres at .5 (OpenGL, Vulkan, Metal,
 * Direct3D 10+) needs no correction. Window y runs downward from the top-left corner of the target; texture
 * coordinates (0,0) are the top-left texel, which is the first row a texture was uploaded with. Colours are
 * 0xAARRGGBB. fog: 1 = no fog, 0 = fully fogged. u0/v0 feed stage 0, u1/v1 stage 1. */
typedef struct { float x, y, z, w; uint32_t diffuse, specular; float fog; float u0, v0, u1, v1; } PlatVertex;
enum { PLAT_PRIM_TRIANGLES, PLAT_PRIM_LINES, PLAT_PRIM_POINTS };
enum { PLAT_BLEND_ZERO, PLAT_BLEND_ONE, PLAT_BLEND_SRC_COLOR, PLAT_BLEND_INV_SRC_COLOR, PLAT_BLEND_SRC_ALPHA, PLAT_BLEND_INV_SRC_ALPHA,
       PLAT_BLEND_DST_ALPHA, PLAT_BLEND_INV_DST_ALPHA, PLAT_BLEND_DST_COLOR, PLAT_BLEND_INV_DST_COLOR, PLAT_BLEND_SRC_ALPHA_SAT };
enum { PLAT_CMP_NEVER, PLAT_CMP_LESS, PLAT_CMP_EQUAL, PLAT_CMP_LEQUAL, PLAT_CMP_GREATER, PLAT_CMP_NOTEQUAL, PLAT_CMP_GEQUAL, PLAT_CMP_ALWAYS };
enum { PLAT_CULL_NONE, PLAT_CULL_CW, PLAT_CULL_CCW };            /* which winding is discarded, as seen on the target (y down) */
/* texture stage operations, a subset of D3DTEXTUREOP with the same meaning */
enum { PLAT_TOP_DISABLE, PLAT_TOP_SELECTARG1, PLAT_TOP_SELECTARG2, PLAT_TOP_MODULATE, PLAT_TOP_MODULATE2X, PLAT_TOP_MODULATE4X, PLAT_TOP_ADD,
       PLAT_TOP_ADDSIGNED, PLAT_TOP_SUBTRACT, PLAT_TOP_BLENDDIFFUSEALPHA, PLAT_TOP_BLENDTEXTUREALPHA, PLAT_TOP_BLENDCURRENTALPHA };
enum { PLAT_ARG_DIFFUSE, PLAT_ARG_CURRENT, PLAT_ARG_TEXTURE, PLAT_ARG_TFACTOR, PLAT_ARG_SPECULAR };
#define PLAT_ARG_COMPLEMENT 0x10      /* or'ed into an argument: 1 - value */
#define PLAT_ARG_ALPHAREPLICATE 0x20  /* or'ed into an argument: alpha in all channels */
enum { PLAT_FILTER_NEAREST, PLAT_FILTER_LINEAR };
enum { PLAT_MIP_NONE, PLAT_MIP_NEAREST, PLAT_MIP_LINEAR };
enum { PLAT_WRAP_REPEAT, PLAT_WRAP_MIRROR, PLAT_WRAP_CLAMP, PLAT_WRAP_BORDER };
typedef struct {
    uint8_t color_op, color_arg1, color_arg2, alpha_op, alpha_arg1, alpha_arg2;
    uint8_t texcoord;              /* which vertex texture coordinate set (0 or 1) */
    uint8_t min_filter, mag_filter, mip_filter, wrap_u, wrap_v, anisotropy;
    uint32_t border_color;
    PlatTexture *texture;          /* NULL: the stage samples opaque white */
} PlatStage;
#define PLAT_MAX_STAGES 2
typedef struct {
    uint8_t blend, src_blend, dst_blend;
    uint8_t alpha_test, alpha_func; uint8_t alpha_ref;
    uint8_t depth_test, depth_write, depth_func;
    uint8_t cull;
    uint8_t fog;                   /* blend toward fog_color by the vertex fog factor (1 = no fog, 0 = full fog) */
    uint8_t specular;              /* add the vertex specular colour after the stages */
    uint8_t flat_shading;
    uint8_t color_write;           /* bit 0 R, 1 G, 2 B, 3 A */
    uint8_t stencil_enable;        /* not used by the engine; backends may ignore */
    uint32_t fog_color, texture_factor;
    float depth_bias;              /* added to window-space depth (D3DRS_DEPTHBIAS); slope bias ignored */
    PlatStage stage[PLAT_MAX_STAGES];
} PlatDrawState;
typedef struct { int x, y, w, h; float min_z, max_z; } PlatViewport;

void plat_gfx_begin_frame(void);
void plat_gfx_set_target(PlatTexture *target);                    /* NULL: the window's back buffer */
void plat_gfx_viewport(const PlatViewport *vp);                   /* pixels in the current target, y down */
void plat_gfx_scissor(int enable, int x, int y, int w, int h);
void plat_gfx_clear(int color, uint32_t argb, int depth, float z, int stencil, uint32_t s);   /* clears the viewport (and scissor) area */
void plat_gfx_draw(int prim, const PlatVertex *v, int nverts, const uint16_t *idx, int nidx, const PlatDrawState *st);
void plat_gfx_copy(PlatTexture *src, int sx, int sy, int sw, int sh, PlatTexture *dst, int dx, int dy, int dw, int dh, int linear);   /* NULL = back buffer */
void plat_gfx_present(void);
void plat_gfx_gamma(const uint16_t ramp[3][256]);                  /* may be ignored */

/* ================================================================ audio */
/* Opens the one output stream: interleaved stereo int16 at `rate` Hz. The callback is called from the backend's audio
 * thread (any thread) and must fill exactly `frames` frames. Returns the rate actually used, or 0 when there is no audio
 * device (the core then runs its mixer on a timer so the game still sees sound buffers progress). */
int  plat_audio_open(int rate, void (*fill)(int16_t *out, int frames, void *user), void *user);
void plat_audio_close(void);
void plat_audio_pause(int paused);

#ifdef __cplusplus
}
#endif
#endif
