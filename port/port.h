/* port.h: the portable host's internal interface. Nothing here is Windows- or x86-specific.
 *
 * The recompiled game calls "imports" (Windows API, C runtime, DirectX) through rt_call_import / rt_call_external.
 * Here every import is a host C function (a "shim") that reads its arguments from the guest stack with A(i), puts its
 * result in eax (RET) or on the x87 stack (RETF), and declares how many bytes of arguments the real function pops
 * (stdcall: 4 per argument, cdecl: 0). Pointers the guest hands over are 32-bit guest addresses; GP() turns one into a
 * host pointer, so every shim is the one place where "guest memory" and "host memory" meet. */
#ifndef PORT_H
#define PORT_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "rt.h"

#define THUNK_BASE 0xF0000000u            /* guest addresses [THUNK_BASE, +16*n) run host functions */
typedef void (*shim_fn)(CPU *c);
typedef struct { const char *name; shim_fn fn; int pop; } ShimDef;   /* pop: bytes of arguments the real function removes */
#define STD(n) (4 * (n))
#define CDECL 0

#define A(i)    RD32(c->esp + 4 + 4 * (i))
#define RET(v)  (c->eax = (uint32_t)(v))
#define RETF(v) FPUSH(c, (double)(v))
#define SHIM(name) static void sh_##name(CPU *c)

/* guest memory */
uint32_t g_alloc(uint32_t n);             /* zeroed; 0 when out of memory */
void     g_free(uint32_t p);
uint32_t g_size(uint32_t p);
uint32_t g_realloc(uint32_t p, uint32_t n);
uint32_t g_str(const char *s);            /* a string that lives as long as the process */
static inline const char *gs(uint32_t a) { return a ? (const char *)GP(a) : ""; }

/* thunks: guest-callable host functions */
uint32_t g_thunk(shim_fn fn, int pop, const char *name);
uint32_t g_thunk_arg(shim_fn fn, int pop, const char *name, uint32_t arg);   /* the shim reads its arg from g_targ */
extern uint32_t g_targ;
extern CPU *g_cpup;
const char *thunk_name(uint32_t addr);
const ShimDef *shim_find(const char *name);
int  thunk_is(uint32_t addr);

/* calling guest code from a shim (callbacks, constructors, window procedures); stdcall or cdecl, result in eax */
uint32_t g_call(CPU *c, uint32_t fn, int nargs, ...);

/* files (Windows paths -> host paths, case-insensitive) */
char *host_path(const char *win, char *out, size_t n);

void port_miss(const char *what, const char *guest, const char *host); /* log a failed file lookup (first 60) */
extern int port_trace;                     /* PORT_TRACE=1: log every import call */
void port_log(const char *fmt, ...);
void port_die(const char *fmt, ...) __attribute__((noreturn));
extern uint32_t g_frames;                  /* Presents seen by the null renderer */
extern uint32_t g_max_frames;              /* stop after this many (0 = never) */
extern uint32_t g_ww, g_wh;                /* size of the one fake window */
void port_exit(int code) __attribute__((noreturn));

/* shim tables, one per area */
int com_slot(const char *iface, const char *method);
/* recompiled DLLs (allobjs.osm, Squirrel.osm, lgvid.dll, fmsel.dll) */
uint32_t mod_load(CPU *c, const char *name);       /* module handle (its base address) or 0 when the name is not a recompiled module */
int      mod_free(CPU *c, uint32_t h);             /* 1 when h was a recompiled module */
uint32_t mod_export(uint32_t h, const char *name, int *is_mod);
int      mod_call(CPU *c, uint32_t target);        /* run target if it lies in a loaded recompiled module */
uint32_t port_proc(const char *name);              /* guest address of a host-implemented function, 0 if unknown */
void port_data_import(unsigned idx, const char *name, uint32_t slot);
extern const ShimDef win32_shims[];
extern const ShimDef crt_shims[];
extern const ShimDef com_shims[];
extern const ShimDef mmio_shims[];
extern const ShimDef win32b_shims[];
#endif
