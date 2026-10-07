/* core.h: the portable host's internal interface, shared by core/, win32/, crt/ and dx/.
 *
 * The recompiled game calls "imports" (Windows API, C runtime, DirectX) through rt_call_import / rt_call_external. Every
 * import is a host C function (a "shim") that reads its arguments from the guest stack with A(i), puts its result in eax
 * (RET) or on the x87 stack (RETF), and declares how many bytes of arguments the real function pops (stdcall: 4 per
 * argument, cdecl: 0). Pointers the guest hands over are 32-bit guest addresses; GP() turns one into a host pointer.
 *
 * Nothing in here, or in the files that include it, calls an operating-system API: that is plat.h's job. */
#ifndef CORE_H
#define CORE_H
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdarg.h>
#include "rt.h"
#include "plat.h"

/* ---------------------------------------------------------------- logging and fatal errors */
void port_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));      /* info */
void port_warn(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void port_debug(const char *fmt, ...) __attribute__((format(printf, 1, 2)));    /* only with --verbose */
void port_die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void port_exit(int code) __attribute__((noreturn));
extern int port_verbose, port_trace;

/* ---------------------------------------------------------------- configuration (command line of the host) */
typedef struct {
    const char *exe_path;          /* host path of the game executable */
    const char *game_dir;          /* host path of the folder that contains it: Windows "C:\" maps here */
    char cmdline[1024];            /* the guest's command line (GetCommandLineA) */
    uint32_t max_frames;           /* stop after this many presented frames (0: never) */
    int windowed;                  /* force windowed mode */
    uint64_t guest_space;          /* bytes of guest address space to reserve (0: the default for this host) */
} PortConfig;
extern PortConfig g_cfg;

/* ---------------------------------------------------------------- shims */
typedef void (*shim_fn)(CPU *c);
typedef struct { const char *name; shim_fn fn; int pop; } ShimDef;   /* pop: bytes of arguments the real function removes */
#define STD(n) (4 * (n))
#define CDECL 0
#define A(i)    RD32(c->esp + 4 + 4 * (i))
#define RET(v)  (c->eax = (uint32_t)(v))
#define RETF(v) FPUSH(c, (double)(v))
#define SHIM(name) static void sh_##name(CPU *c)
#define S(n, p) { #n, sh_##n, p }
#define SA(name, impl, p) { name, sh_##impl, p }
static inline void ret64(CPU *c, uint64_t v) { c->eax = (uint32_t)v; c->edx = (uint32_t)(v >> 32); }
/* the shim tables, one per module; port_shim_tables() lists them, NULL-terminated */
extern const ShimDef kernel32_shims[], kernel32_file_shims[], user32_shims[], gdi32_shims[], advapi32_shims[], winmm_shims[], misc_shims[];
extern const ShimDef msvcrt_shims[], msvcrt_stdio_shims[], cxx_shims[];
extern const ShimDef d3d9_shims[], dsound_shims[], dinput_shims[], ddraw_shims[];
const ShimDef *shim_find(const char *name);

/* ---------------------------------------------------------------- guest memory */
extern uint64_t g_space;                    /* bytes of guest address space backed by M */
static inline int g_valid(uint32_t a, uint32_t n) { return a >= 0x10000u && (uint64_t)a + n <= g_space; }
static inline const char *gs(uint32_t a) { return a && g_valid(a, 1) ? (const char *)GP(a) : ""; }
/* address-space regions, 64 KB granularity (VirtualAlloc, stacks, heap arenas). want = 0: anywhere. 0 on failure. */
uint32_t vm_alloc(uint32_t size, uint32_t want, const char *what);
uint32_t vm_alloc_quiet(uint32_t size);     /* the same, anywhere, without a warning when it fails */
void     vm_free(uint32_t addr);
uint32_t vm_region_size(uint32_t addr);     /* size of the region starting at addr, 0 if none */
int      vm_mark(uint32_t lo, uint32_t hi, const char *what);    /* reserve a fixed range (images); 0 if it overlaps */
/* the guest heap (malloc, HeapAlloc, operator new): zero-filled, 16-byte aligned, O(1) allocation with coalescing */
uint32_t g_alloc(uint32_t n);
void     g_free(uint32_t p);
uint32_t g_size(uint32_t p);                /* the size that was asked for */
uint32_t g_realloc(uint32_t p, uint32_t n);
int      g_owns(uint32_t p);                /* p is a live heap block */
uint32_t g_str(const char *s);              /* a copy of s in guest memory (caller frees with g_free, or keeps it) */
void     heap_check(void);                  /* validates every block; dies on corruption */

/* ---------------------------------------------------------------- thunks: guest-callable host functions */
#define THUNK_BASE 0xF0000000u              /* guest addresses [THUNK_BASE, +16*n) run host functions */
uint32_t g_thunk(shim_fn fn, int pop, const char *name);
uint32_t g_thunk_arg(shim_fn fn, int pop, const char *name, uint32_t arg);   /* the shim reads arg from g_targ */
extern uint32_t g_targ;
const char *thunk_name(uint32_t addr);
int  thunk_is(uint32_t addr);
uint32_t port_proc(const char *name);       /* thunk of a host-implemented export (GetProcAddress), 0 if unknown */

/* calling guest code from a shim (callbacks, constructors, window procedures); stdcall or cdecl, result in eax */
uint32_t g_call(CPU *c, uint32_t fn, int nargs, ...);
uint32_t g_callv(CPU *c, uint32_t fn, int nargs, const uint32_t *args);

/* ---------------------------------------------------------------- threads
 * Guest threads are real host threads, but only one runs guest code at a time: it holds the guest lock. A thread gives
 * the lock up while it blocks (Sleep, waits, contended critical sections, the message wait) and when another thread
 * has waited a whole time slice (the generated code checks rt_preempt_req at every loop head, see rt.h). So shims and
 * host state need no locking of their own, except state shared with non-guest host threads (the audio mixer). */
typedef struct GuestThread GuestThread;
extern GuestThread *g_cur;                  /* the thread that holds the guest lock */
CPU *cur_cpu(void);
uint32_t cur_thread_id(void);
uint32_t cur_teb(void);
void gil_acquire(GuestThread *t);
void gil_release(void);
#define BLOCKING(stmt) do { GuestThread *t_ = g_cur; gil_release(); stmt; gil_acquire(t_); } while (0)
/* a guest thread that is not created by the game: the WINMM timer thread, for example */
GuestThread *thread_new_host_side(const char *name);
GuestThread *thread_main_init(void);
void thread_preempt_tick(void);             /* called at import boundaries */

/* ---------------------------------------------------------------- kernel objects and handles */
typedef enum { K_NONE, K_FILE, K_EVENT, K_MUTEX, K_SEMAPHORE, K_THREAD, K_FIND, K_MAPPING, K_PROCESS, K_OTHER } KType;
typedef struct KObj {
    KType type; int refs;
    void (*destroy)(struct KObj *);
    int (*signaled)(struct KObj *, GuestThread *t);     /* for waits: can t take it now? */
    void (*acquire)(struct KObj *, GuestThread *t);     /* t's wait was satisfied (reset an auto event, own a mutex, ...) */
} KObj;
uint32_t handle_new(KObj *o);               /* takes the caller's reference */
KObj    *handle_get(uint32_t h, KType type); /* borrowed; NULL if h is not a handle of that type (K_NONE: any) */
uint32_t handle_dup(uint32_t h);
int      handle_close(uint32_t h);
void     kobj_ref(KObj *o);
void     kobj_unref(KObj *o);
/* waits for any (all = 0) or all of n objects; timeout_ms 0xFFFFFFFF = forever. Returns WAIT_OBJECT_0 + i, WAIT_TIMEOUT,
 * WAIT_FAILED. `alertable_msgs`: also return n when a window message arrives (MsgWaitForMultipleObjects). */
uint32_t kwait(KObj **objs, int n, int all, uint32_t timeout_ms, int alertable_msgs);
void     kobj_changed(void);                /* call after any state change that could satisfy a wait */
/* events, used by DirectSound notifications and the message queue too */
KObj *kevent_new(int manual, int initial);
void  kevent_set(KObj *e);
void  kevent_reset(KObj *e);

/* ---------------------------------------------------------------- per-thread state the Win32 shims need */
void     set_last_error(uint32_t e);
uint32_t get_last_error(void);

/* ---------------------------------------------------------------- files: Windows paths to host paths */
/* Maps a Windows path ("C:\Games\SS2\Data\res\a.crf", "Data\res", "./a.txt") to a host path below the game folder,
 * resolving case differences against what exists. Returns out. For a path that does not exist yet, the existing part is
 * case-resolved and the rest kept as given. */
char *vfs_map(const char *win, char *out, size_t n);
char *vfs_map_write(const char *win, char *out, size_t n);     /* the same, for a file the game writes (the write folder, if any) */
PlatFile *vfs_open(const char *win, int flags, int *err, char *host, size_t hn);
void  vfs_changed(const char *host_path);   /* after creating, removing or renaming: drops cached directory listings */
int   vfs_copy(const char *from_host, const char *to_host);
int   vfs_chdir(const char *win);
const char *vfs_cwd(void);
char *vfs_full_win(const char *win, char *out, size_t n);
int   vfs_err_to_win(int plat_err);         /* PLAT_E_* -> ERROR_* */
void  port_miss(const char *what, const char *guest);    /* log a failed lookup (first 60, or all with --verbose) */
int   win_glob(const char *pattern, const char *name);   /* Windows wildcard match (*, ?), case-insensitive */

/* ---------------------------------------------------------------- input and window state shared by user32 / dinput */
typedef struct {
    uint8_t keys[256];             /* DIK scan code -> down */
    int mouse_x, mouse_y;          /* client coordinates, in the game's resolution */
    int mouse_dx, mouse_dy, wheel; /* accumulated since the last DirectInput read */
    uint8_t buttons[5];
    int focused;
    uint32_t hwnd;                 /* the game window (0 before CreateWindowExA) */
} InputState;
extern InputState g_input;
void input_pump(void);             /* polls the platform and turns events into window messages / DirectInput data */
typedef void (*input_listener)(const PlatEvent *ev);
void input_listen(input_listener fn);     /* DirectInput hooks in here for buffered data */
int  dik_to_vk(int dik);

/* ---------------------------------------------------------------- recompiled DLLs (core/modules.c) */
uint32_t mod_load(CPU *c, const char *name);    /* module handle (its base address) or 0 when the name is not a recompiled module */
int      mod_free(CPU *c, uint32_t h);          /* 1 when h was a recompiled module */
uint32_t mod_export(uint32_t h, const char *name, int *is_mod);
uint32_t mod_handle_of(const char *name);
int      mod_call(CPU *c, uint32_t target);     /* run target if it lies in a loaded recompiled module */
void     mod_reserve_all(void);                 /* marks every recompiled module's address range as used */

/* ---------------------------------------------------------------- misc */
#include <stdlib.h>
static inline char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *d = (char *)malloc(n); memcpy(d, s, n); return d; }
static inline int _stricmp_ascii(const char *a, const char *b) { int d; do { int x = (unsigned char)*a, y = (unsigned char)*b; if (x >= 'A' && x <= 'Z') x += 32; if (y >= 'A' && y <= 'Z') y += 32; d = x - y; } while (!d && *a++ && *b++); return d; }
uint32_t ms_ticks(void);                         /* GetTickCount */
uint64_t unix_ns_to_filetime(int64_t ns);
extern uint32_t g_frames;                        /* frames presented */
void port_frame_presented(void);                 /* counts, honours --frames, pumps input */
const char *guest_symbol(uint32_t va);           /* function name from the symbol files if built in, else NULL */
#endif
