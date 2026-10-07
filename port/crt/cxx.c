/* cxx.c: the C++ part of the runtime the game imports: operator new/delete, std::exception, type_info, the few
 * std::string members it takes from MSVCP90 (VC9 layout), and the exception entry points (fatal for now: the engine
 * throws in one place only, see NEWDARK.md). */
#include <stdio.h>
#include <stdlib.h>
#include "crt.h"

SHIM(op_new) { uint32_t p = g_alloc(A(0) ? A(0) : 1); if (!p) port_die("operator new(%u): out of guest memory", A(0)); RET(p); }
SHIM(op_delete) { g_free(A(0)); }
SHIM(die_cxx) { port_die("a C++ exception was thrown (_CxxThrowException); exceptions are not supported by the portable host yet"); }
SHIM(die_seh) { port_die("structured/C++ exception dispatch was entered; exceptions are not supported by the portable host yet"); }
SHIM(die_terminate) { port_die("std::terminate was called"); }
SHIM(die_setjmp) { port_die("setjmp/longjmp reached the host: the lifter should have translated this call"); }
SHIM(zero) { RET(0); }
SHIM(nop) { }

/* ---------------------------------------------------------------- std::exception (VC9: vftable, const char *_m_what, int _m_doFree) */
static uint32_t exc_vtable(void);
SHIM(exc_ctor0) { uint32_t t = c->ecx; WR32(t, exc_vtable()); WR32(t + 4, 0); WR32(t + 8, 0); RET(t); }
SHIM(exc_ctor_str) {        /* exception(const char *const &): copies the message */
    uint32_t t = c->ecx, pp = A(0), s = pp ? RD32(pp) : 0; WR32(t, exc_vtable());
    if (s) { WR32(t + 4, g_str(gs(s))); WR32(t + 8, 1); } else { WR32(t + 4, 0); WR32(t + 8, 0); } RET(t);
}
SHIM(exc_ctor_copy) { uint32_t t = c->ecx, o = A(0); WR32(t, exc_vtable()); uint32_t s = RD32(o + 4); if (s && RD32(o + 8)) { WR32(t + 4, g_str(gs(s))); WR32(t + 8, 1); } else { WR32(t + 4, s); WR32(t + 8, 0); } RET(t); }
SHIM(exc_dtor) { uint32_t t = c->ecx; if (RD32(t + 8) && RD32(t + 4)) g_free(RD32(t + 4)); WR32(t + 4, 0); WR32(t + 8, 0); }
SHIM(exc_what) { uint32_t s = RD32(c->ecx + 4); static uint32_t unk; if (!unk) unk = g_str("Unknown exception"); RET(s ? s : unk); }
SHIM(exc_vdtor) { uint32_t t = c->ecx; if (RD32(t + 8) && RD32(t + 4)) g_free(RD32(t + 4)); if (A(0) & 1) g_free(t); RET(t); }    /* scalar deleting destructor */
static uint32_t exc_vtable(void) {
    static uint32_t vt; if (vt) return vt;
    vt = g_alloc(8); WR32(vt, g_thunk(sh_exc_vdtor, STD(1), "std::exception::`scalar deleting destructor'")); WR32(vt + 4, g_thunk(sh_exc_what, 0, "std::exception::what"));
    return vt;
}
SHIM(typeinfo_dtor) { }

/* ---------------------------------------------------------------- std::string (VC9: 28 bytes; +4 union { char buf[16]; char *ptr; }, +20 size, +24 capacity) */
#define SS_BUF(s) ((s) + 4)
#define SS_SIZE(s) ((s) + 20)
#define SS_RES(s) ((s) + 24)
static uint32_t ss_data(uint32_t s) { return RD32(SS_RES(s)) >= 16 ? RD32(SS_BUF(s)) : SS_BUF(s); }
static void ss_init(uint32_t s) { memset(GP(s), 0, 28); WR32(SS_RES(s), 15); }
static void ss_assign(uint32_t s, const char *p, uint32_t n) {     /* p must not point into s */
    if (n > RD32(SS_RES(s))) {
        uint32_t cap = n | 15; uint32_t d = g_alloc(cap + 1);
        if (RD32(SS_RES(s)) >= 16) g_free(RD32(SS_BUF(s)));
        WR32(SS_BUF(s), d); WR32(SS_RES(s), cap);
    }
    uint32_t d = ss_data(s); memmove(GP(d), p, n); WR8(d + n, 0); WR32(SS_SIZE(s), n);
}
static char *copy_of(uint32_t g, uint32_t n) { char *t = malloc(n + 1); memcpy(t, GP(g), n); t[n] = 0; return t; }
SHIM(ss_ctor0) { ss_init(c->ecx); RET(c->ecx); }
SHIM(ss_ctor_cstr) { uint32_t s = c->ecx; char *t = xstrdup(gs(A(0))); ss_init(s); ss_assign(s, t, (uint32_t)strlen(t)); free(t); RET(s); }
SHIM(ss_ctor_copy) { uint32_t s = c->ecx, o = A(0); uint32_t n = RD32(SS_SIZE(o)); char *t = copy_of(ss_data(o), n); ss_init(s); ss_assign(s, t, n); free(t); RET(s); }
SHIM(ss_dtor) { uint32_t s = c->ecx; if (RD32(SS_RES(s)) >= 16) g_free(RD32(SS_BUF(s))); WR32(SS_RES(s), 15); WR32(SS_SIZE(s), 0); WR8(SS_BUF(s), 0); }
SHIM(ss_assign_cstr) { uint32_t s = c->ecx; char *t = xstrdup(gs(A(0))); ss_assign(s, t, (uint32_t)strlen(t)); free(t); RET(s); }
SHIM(ss_append_cstr) {
    uint32_t s = c->ecx; const char *p = gs(A(0)); uint32_t l = (uint32_t)strlen(p), n = RD32(SS_SIZE(s)); char *t = malloc(n + l + 1);
    memcpy(t, GP(ss_data(s)), n); memcpy(t + n, p, l); ss_assign(s, t, n + l); free(t); RET(s);
}
/* begin()/end() return a _String_iterator by value: the caller passes a hidden result pointer (popped by the callee) and
 * gets it back in eax. VC9's checked iterators are { container, pointer }. Nothing has exercised these yet. */
static void iter_out(CPU *c, uint32_t ptr) {
    static int warned; if (!warned++) port_debug("std::string::begin/end used (iterator layout { container, pointer })");
    uint32_t r = A(0); WR32(r, c->ecx); WR32(r + 4, ptr); RET(r);
}
SHIM(ss_begin) { iter_out(c, ss_data(c->ecx)); }
SHIM(ss_end) { iter_out(c, ss_data(c->ecx) + RD32(SS_SIZE(c->ecx))); }
SHIM(ss_eq) { uint32_t a = A(0), b = A(1); RET(RD32(SS_SIZE(a)) == RD32(SS_SIZE(b)) && !memcmp(GP(ss_data(a)), GP(ss_data(b)), RD32(SS_SIZE(a)))); }
SHIM(ss_lt) { uint32_t a = A(0), b = A(1); uint32_t la = RD32(SS_SIZE(a)), lb = RD32(SS_SIZE(b)); int r = memcmp(GP(ss_data(a)), GP(ss_data(b)), la < lb ? la : lb); RET(r < 0 || (r == 0 && la < lb)); }
SHIM(alloc_allocate) { RET(g_alloc(A(0) ? A(0) : 1)); }
SHIM(alloc_deallocate) { g_free(A(0)); }

#define SSN "?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@std@@"
const ShimDef cxx_shims[] = {
    SA("??2@YAPAXI@Z", op_new, CDECL), SA("??3@YAXPAX@Z", op_delete, CDECL), SA("??_U@YAPAXI@Z", op_new, CDECL), SA("??_V@YAXPAX@Z", op_delete, CDECL),
    SA("_CxxThrowException", die_cxx, STD(2)), SA("__CxxFrameHandler3", die_seh, CDECL), SA("_except_handler4_common", die_seh, CDECL), SA("_except_handler3", die_seh, CDECL),
    SA("__CppXcptFilter", zero, CDECL), SA("?terminate@@YAXXZ", die_terminate, CDECL), SA("?_type_info_dtor_internal_method@type_info@@QAEXXZ", typeinfo_dtor, 0),
    SA("__clean_type_info_names_internal", nop, CDECL), SA("_setjmp3", die_setjmp, CDECL), SA("_setjmp", die_setjmp, CDECL), SA("longjmp", die_setjmp, CDECL),
    SA("??0exception@std@@QAE@XZ", exc_ctor0, 0), SA("??0exception@std@@QAE@ABQBD@Z", exc_ctor_str, STD(1)), SA("??0exception@std@@QAE@ABV01@@Z", exc_ctor_copy, STD(1)),
    SA("??1exception@std@@UAE@XZ", exc_dtor, 0), SA("?what@exception@std@@UBEPBDXZ", exc_what, 0),
    SA("?begin@" SSN "QAE?AV?$_String_iterator@DU?$char_traits@D@std@@V?$allocator@D@2@@2@XZ", ss_begin, STD(1)),
    SA("?end@" SSN "QAE?AV?$_String_iterator@DU?$char_traits@D@std@@V?$allocator@D@2@@2@XZ", ss_end, STD(1)),
    SA("??0" SSN "QAE@PBD@Z", ss_ctor_cstr, STD(1)), SA("??0" SSN "QAE@ABV01@@Z", ss_ctor_copy, STD(1)), SA("??0" SSN "QAE@XZ", ss_ctor0, 0), SA("??1" SSN "QAE@XZ", ss_dtor, 0),
    SA("??Y" SSN "QAEAAV01@PBD@Z", ss_append_cstr, STD(1)), SA("??4" SSN "QAEAAV01@PBD@Z", ss_assign_cstr, STD(1)),
    SA("??$?8DU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", ss_eq, CDECL),
    SA("??$?MDU?$char_traits@D@std@@V?$allocator@D@1@@std@@YA_NABV?$basic_string@DU?$char_traits@D@std@@V?$allocator@D@2@@0@0@Z", ss_lt, CDECL),
    SA("?allocate@?$allocator@D@std@@QAEPADI@Z", alloc_allocate, STD(1)), SA("?deallocate@?$allocator@D@std@@QAEXPADI@Z", alloc_deallocate, STD(2)),
    { 0, 0, 0 }
};
