/* test program for lift.py --cache-regs with the calling-convention contract.
 * COM-like interfaces called through vtables, callbacks, imports, and register pressure around those calls, so that
 * EBX/ESI/EDI/EBP are live across indirect calls. Two negative controls break the rules on purpose:
 * bad_callee clobbers EBX/ESI behind a function pointer (breaks the calling-convention contract), and smash_callee saves
 * EBX and then overwrites the saved copy through a pointer the lifter cannot follow (breaks the assumption under the
 * preservation proof, lift_preserve.py), so its "restores EBX" claim is wrong. */
#include <windows.h>
#include <stdlib.h>
#include <string.h>

typedef struct Obj Obj;
typedef struct { int (__thiscall *get)(Obj *); void (__thiscall *add)(Obj *, int); int (__stdcall *mix)(int, int); } Vt;
struct Obj { const Vt *vt; int v; };

static int __thiscall o_get(Obj *o) { return o->v; }
static void __thiscall o_add(Obj *o, int d) { o->v += d * 3 + 1; }
static int __stdcall o_mix(int a, int b) { return (a * 31) ^ (b >> 2); }
static int __thiscall p_get(Obj *o) { return o->v * 2 - 7; }
static void __thiscall p_add(Obj *o, int d) { o->v ^= d << 1; }
static int __stdcall p_mix(int a, int b) { return a - b * 5; }

static const Vt vt_o = { o_get, o_add, o_mix }, vt_p = { p_get, p_add, p_mix };
volatile int sink;

/* many values live across every indirect call: the compiler keeps them in ebx/esi/edi/ebp */
__declspec(noinline) int walk(Obj **objs, int n, int seed) {
    int a = seed, b = seed * 7, c = seed ^ 0x55, d = n;
    for (int i = 0; i < n; i++) {
        Obj *o = objs[i];
        int g = o->vt->get(o);
        a += g; b ^= a; c += b >> 3; d -= c & 15;
        o->vt->add(o, a & 31);
        a = o->vt->mix(a, b) + c;
        b += d;
    }
    return a ^ b ^ c ^ d;
}

typedef int (*cmpfn)(const void *, const void *);
static int cmp_int(const void *x, const void *y) { return *(const int *)x - *(const int *)y; }

__declspec(noinline) int sort_and_sum(int *v, int n, cmpfn f) {
    int s1 = 0, s2 = 1, s3 = 3;
    qsort(v, n, sizeof *v, f);                       /* import (msvcrt) that calls back into guest code */
    for (int i = 0; i < n; i++) { s1 += v[i]; s2 = s2 * 3 + v[i]; s3 ^= s1 + s2; }
    s3 += (int)strlen("darkrecomp");                  /* import */
    s3 += (int)(GetTickCount() & 0);                  /* import (stdcall) */
    return s1 + s2 + s3;
}

/* negative control: reached only through a pointer, clobbers ebx and esi without restoring them */
__attribute__((naked)) int bad_callee(void) {
    __asm__("movl $0x11111111, %ebx\n\tmovl $0x22222222, %esi\n\tmovl $5, %eax\n\tret");
}
int (*volatile bad_ptr)(void) = bad_callee;

__declspec(noinline) int use_bad(int x) {
    int a = x * 3, b = x ^ 9, c = x + 100;            /* live across the call: in callee-saved registers */
    int r = bad_ptr();
    return r + a + b * 2 + c * 3;
}

/* negative control for the preservation proof: saves EBX, then writes over the saved copy through a pointer laundered
 * through memory, so the proof (which cannot follow it) wrongly concludes that EBX is restored */
volatile unsigned smash_ptr;
__attribute__((naked)) int smash_callee(void) {
    __asm__("pushl %ebx\n\tmovl $0x33333333, %ebx\n\tmovl %esp, smash_ptr\n\tmovl smash_ptr, %ecx\n\t"
            "movl $0x77777777, (%ecx)\n\tpopl %ebx\n\tmovl $9, %eax\n\tret");
}
volatile int smash_in[4] = { 3, 5, 7, 11 };
__declspec(noinline) int use_smash(int x) {
    /* four values the compiler cannot recompute after the call (volatile reads), so EBX holds one of them */
    int a = smash_in[0] * x, b = smash_in[1] ^ x, c = smash_in[2] + x, d = smash_in[3] - x;
    int r = smash_callee();
    return r + a + b * 3 + c * 7 + d * 11;
}
__declspec(dllexport) __declspec(noinline) int test_smash(int seed) { return use_smash(seed); }

__declspec(dllexport) __declspec(noinline) int test_good(int seed) {
    Obj o[6]; Obj *p[6];
    for (int i = 0; i < 6; i++) { o[i].vt = (i & 1) ? &vt_p : &vt_o; o[i].v = i * 11 + seed; p[i] = &o[i]; }
    int v[16]; for (int i = 0; i < 16; i++) v[i] = (i * 7919 + seed) % 97;
    return walk(p, 6, seed) * 3 + sort_and_sum(v, 16, cmp_int);
}
__declspec(dllexport) __declspec(noinline) int test_bad(int seed) { return use_bad(seed); }

int main(int argc, char **argv) { (void)argv; sink = test_good(argc) + test_bad(argc) + test_smash(argc); return sink & 0xFF; }
