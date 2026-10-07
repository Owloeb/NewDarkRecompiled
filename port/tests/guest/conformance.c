/* conformance.c: a guest program for the portable host's conformance tests.
 *
 * Built as a 32-bit x86 Windows executable (freestanding, no C library of its own), lifted to C with lift.py and run on the
 * portable host exactly like SS2.exe. It calls the same imports the game uses (MSVCR90, KERNEL32, USER32, WINMM,
 * Direct3D 9, DirectSound, DirectInput) and checks the results against what Windows and the Microsoft C runtime return,
 * so the platform layer can be tested anywhere without the game's files.
 *
 * Each check prints "FAIL <section>: <expression>" when it fails; the exit code is the number of failures. Sections can
 * be skipped by name on the command line: conformance.exe -skip threads -skip window */
#define WIN32_LEAN_AND_MEAN
#define CINTERFACE
#define COBJMACROS
#include <windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <d3d9.h>
#include <dsound.h>
#define DIRECTINPUT_VERSION 0x0700
#include <dinput.h>
#include "crt90.h"
/* MinGW's headers make the Interlocked functions inline lock-prefixed instructions; the game imports them from KERNEL32
   (and the recompiler does not translate xadd/cmpxchg), so call the imports explicitly */
__declspec(dllimport) LONG WINAPI k32_InterlockedIncrement(LONG volatile *) __asm__("_InterlockedIncrement@4");
__declspec(dllimport) LONG WINAPI k32_InterlockedDecrement(LONG volatile *) __asm__("_InterlockedDecrement@4");
__declspec(dllimport) LONG WINAPI k32_InterlockedExchange(LONG volatile *, LONG) __asm__("_InterlockedExchange@8");
__declspec(dllimport) LONG WINAPI k32_InterlockedCompareExchange(LONG volatile *, LONG, LONG) __asm__("_InterlockedCompareExchange@12");
#undef InterlockedIncrement
#undef InterlockedDecrement
#undef InterlockedExchange
#undef InterlockedCompareExchange
#define InterlockedIncrement k32_InterlockedIncrement
#define InterlockedDecrement k32_InterlockedDecrement
#define InterlockedExchange k32_InterlockedExchange
#define InterlockedCompareExchange k32_InterlockedCompareExchange

/* ---------------------------------------------------------------- tiny test framework */
static int fails, checks; static const char *section;
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void out(const char *s) { DWORD w; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, slen(s), &w, NULL); }
static void check(int ok, const char *what, int line) {
    checks++; if (ok) return; fails++; char b[512]; sprintf(b, "FAIL %s (line %d): %s\n", section, line, what); out(b);
}
#define CHECK(x) check(!!(x), #x, __LINE__)
#define CHECKS(a, b) do { const char *a_ = (a), *b_ = (b); int ok_ = a_ && seq(a_, b_); check(ok_, #a " == \"" b "\"", __LINE__); if (!ok_) { char t_[400]; _snprintf(t_, 399, "     got \"%s\"\n", a_ ? a_ : "(null)"); t_[399] = 0; out(t_); } } while (0)
#define CHECKI(a, b) do { long a_ = (long)(a), b_ = (long)(b); check(a_ == b_, #a " == " #b, __LINE__); if (a_ != b_) { char t_[200]; sprintf(t_, "     got %ld (0x%lx), expected %ld\n", a_, a_, b_); out(t_); } } while (0)

/* calls fn as stdcall with n arguments and reports how many bytes it popped wrongly (0 = correct ABI) */
int __cdecl stdcall_check(void *fn, int n, const unsigned *args, unsigned *ret);
__asm__(".globl _stdcall_check\n_stdcall_check:\n"
        "  pushl %ebp\n  movl %esp, %ebp\n  pushl %esi\n  pushl %edi\n  pushl %ebx\n"
        "  movl 8(%ebp), %eax\n  movl 12(%ebp), %ecx\n  movl 16(%ebp), %edx\n  movl %esp, %esi\n"
        "1: testl %ecx, %ecx\n  jz 2f\n  decl %ecx\n  pushl (%edx,%ecx,4)\n  jmp 1b\n"
        "2: call *%eax\n  movl 20(%ebp), %ecx\n  movl %eax, (%ecx)\n  movl %esi, %eax\n  subl %esp, %eax\n  movl %esi, %esp\n"
        "  popl %ebx\n  popl %edi\n  popl %esi\n  popl %ebp\n  ret\n");
/* COM method by vtable slot, checked for the right stack cleanup: n arguments after `this` */
static unsigned vcall(void *obj, int slot, int n, unsigned a1, unsigned a2, unsigned a3, unsigned a4, unsigned a5, unsigned a6, unsigned a7) {
    unsigned args[8] = { (unsigned)obj, a1, a2, a3, a4, a5, a6, a7 }, r = 0; void *fn = (*(void ***)obj)[slot];
    int bad = stdcall_check(fn, n + 1, args, &r);
    if (bad) { char b[160]; sprintf(b, "     vtable slot %d popped %d bytes too %s\n", slot, bad < 0 ? -bad : bad, bad > 0 ? "few" : "many"); out(b); }
    check(!bad, "COM call cleans up its own arguments", slot);
    return r;
}
/* x87 helpers: _CI functions take st(0) [and st(1)] and leave the result in st(0) */
double __cdecl ci1(double a, void *fn);
double __cdecl ci2(double a, double b, void *fn);
__asm__(".globl _ci1\n_ci1:\n  fldl 4(%esp)\n  call *12(%esp)\n  ret\n"
        ".globl _ci2\n_ci2:\n  fldl 4(%esp)\n  fldl 12(%esp)\n  call *20(%esp)\n  ret\n");
static int close_to(double a, double b) { double d = a - b; return d < 1e-9 && d > -1e-9; }
static int skip_mask; static const char *const names[] = { "crt", "stdio", "win32", "threads", "window", "d3d9", "dsound", "dinput", "pixels", 0 };
static int on(int i) { section = names[i]; return !(skip_mask & (1 << i)); }

/* ---------------------------------------------------------------- C runtime: formatting, parsing, strings, sorting, math */
static int __cdecl cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static int nested_calls;
static int __cdecl cmp_nested(const void *a, const void *b) {        /* a comparator that sorts something itself (reentrancy) */
    int t[5] = { 5, 1, 4, 2, 3 }; qsort(t, 5, sizeof *t, cmp_int); nested_calls += t[0] == 1 && t[4] == 5;
    return *(const int *)a - *(const int *)b;
}
static void test_crt(void) {
    char b[256];
    CHECKI(sprintf(b, "%d|%5d|%-5d|%05d|%u|%x|%X|%08x", -42, 7, 7, 7, 3000000000u, 255, 255, 0xbeef), 47); CHECKS(b, "-42|    7|7    |00007|3000000000|ff|FF|0000beef");
    sprintf(b, "%s|%10s|%-4s|%.2s|%c|%%", "abc", "right", "l", "trunc", 'Z'); CHECKS(b, "abc|     right|l   |tr|Z|%");
    sprintf(b, "%f|%.2f|%8.3f|%g|%g", 3.14159, 2.005, -1.5, 0.0001, 100000.0); CHECKS(b, "3.141590|2.00|  -1.500|0.0001|100000");
    sprintf(b, "%e|%.2E|%g", 1.5, 12345.678, 1e20); CHECKS(b, "1.500000e+000|1.23E+004|1e+020");      /* MSVC: three exponent digits */
    sprintf(b, "%I64d|%I64x|%lld", (long long)-5000000000ll, 0x123456789ull, 42ll); CHECKS(b, "-5000000000|123456789|42");
    sprintf(b, "%hd|%ld|%p", 70000, 5l, (void *)0x1234); CHECKS(b, "4464|5|00001234");
    CHECKI(_snprintf(b, 5, "123456"), -1); CHECK(b[0] == '1' && b[4] == '5');
    b[5] = 'X'; CHECKI(_snprintf(b, 5, "12345"), 5); CHECK(b[5] == 'X');
    CHECKI(_snprintf(b, 10, "%d", 12), 2); CHECKS(b, "12");

    int i1 = 0, i2 = 0; float f1 = 0; double d1 = 0; char s1[32] = "", s2[32] = ""; unsigned x1 = 0;
    CHECKI(sscanf("12 3.5 abc,def", "%d %f %[^,],%s", &i1, &f1, s1, s2), 4); CHECKI(i1, 12); CHECK(f1 == 3.5f); CHECKS(s1, "abc"); CHECKS(s2, "def");
    CHECKI(sscanf("ff -7 2.25", "%x %d %lf", &x1, &i2, &d1), 3); CHECKI(x1, 255); CHECKI(i2, -7); CHECK(d1 == 2.25);
    CHECKI(sscanf("", "%d", &i1), -1); CHECKI(sscanf("x", "%d", &i1), 0);
    CHECKI(sscanf("key = value", "%s = %s", s1, s2), 2); CHECKS(s1, "key"); CHECKS(s2, "value");

    char tk[] = "a, b,,c"; char *t = strtok(tk, ", "); CHECKS(t, "a"); t = strtok(0, ", "); CHECKS(t, "b"); t = strtok(0, ", "); CHECKS(t, "c"); CHECK(strtok(0, ", ") == 0);
    char *e; CHECKI(strtol("  -0x1fz", &e, 16), -31); CHECK(*e == 'z'); CHECKI(strtoul("4294967295", 0, 10), (long)4294967295u); CHECKI(atoi("  123abc"), 123);
    CHECK(atof("2.5e2") == 250.0); CHECKS(_itoa(-255, b, 16), "ffffff01"); CHECKS(_itoa(-255, b, 10), "-255");
    CHECKI(_stricmp("Hello", "hELLO"), 0); CHECK(_stricmp("a", "B") < 0); CHECKI(_strnicmp("abcX", "ABCY", 3), 0); CHECK(strncmp("abc", "abd", 3) < 0);
    const char *hs = "path\\to\\file.ext"; CHECK(strrchr(hs, '\\') == hs + 7); CHECK(strchr(hs, '.') == hs + 12); CHECK(strstr(hs, "to") == hs + 5); CHECK(memchr(hs, 'f', 16) == hs + 8);
    char *dup = _strdup("MiXeD"); CHECKS(_strlwr(dup), "mixed"); free(dup);
    char nb[8]; memset(nb, 'q', 8); strncpy(nb, "ab", 5); CHECK(nb[2] == 0 && nb[4] == 0 && nb[5] == 'q');
    char mv[] = "abcdef"; memmove(mv + 1, mv, 4); CHECKS(mv, "aabcdf");

    int arr[9] = { 5, 3, 9, 1, 7, 2, 8, 6, 4 }; qsort(arr, 9, sizeof *arr, cmp_int); int sorted = 1; for (int i = 0; i < 9; i++) sorted &= arr[i] == i + 1; CHECK(sorted);
    int key = 7; int *hit = bsearch(&key, arr, 9, sizeof *arr, cmp_int); CHECK(hit == &arr[6]); key = 10; CHECK(bsearch(&key, arr, 9, sizeof *arr, cmp_int) == 0);
    int arr2[6] = { 3, 1, 2, 6, 5, 4 }; nested_calls = 0; qsort(arr2, 6, sizeof *arr2, cmp_nested); CHECK(arr2[0] == 1 && arr2[5] == 6); CHECK(nested_calls > 0);
    static int big[2000]; for (int i = 0; i < 2000; i++) big[i] = (i * 7919) % 2000; qsort(big, 2000, sizeof *big, cmp_int); sorted = 1; for (int i = 0; i < 2000; i++) sorted &= big[i] == i; CHECK(sorted);

    srand(1); CHECKI(rand(), 41); CHECKI(rand(), 18467); CHECKI(rand(), 6334); srand(12345); CHECKI(rand(), 7584);
    CHECK(floor(-1.5) == -2.0); CHECK(ceil(1.25) == 2.0); CHECK(ldexp(3.0, 4) == 48.0);
    CHECK(close_to(ci2(2.0, 10.0, (void *)_CIpow), 1024.0)); CHECK(close_to(ci1(81.0, (void *)_CIsqrt), 9.0));
    CHECK(close_to(ci2(1.0, 1.0, (void *)_CIatan2), 0.78539816339744828)); CHECK(close_to(ci2(7.5, 2.0, (void *)_CIfmod), 1.5));

    /* heap: sizes, zero-fill of calloc, realloc keeping data, and a randomized stress run with guard patterns */
    char *p = malloc(100); CHECK(p != 0); CHECK(_msize(p) == 100); for (int i = 0; i < 100; i++) p[i] = (char)i;
    p = realloc(p, 5000); CHECK(p != 0); int keep = 1; for (int i = 0; i < 100; i++) keep &= p[i] == (char)i; CHECK(keep); free(p);
    int *z = calloc(50, 4); int zero = 1; for (int i = 0; i < 50; i++) zero &= z[i] == 0; CHECK(zero); free(z);
    static unsigned char *slots[512]; static unsigned sizes[512]; unsigned seed = 99; int intact = 1;
    for (int round = 0; round < 20000; round++) {
        seed = seed * 1103515245u + 12345u; unsigned k = (seed >> 8) % 512;
        if (slots[k]) { for (unsigned i = 0; i < sizes[k]; i++) intact &= slots[k][i] == (unsigned char)(k ^ i); free(slots[k]); slots[k] = 0; }
        else { sizes[k] = (seed >> 16) % ((seed & 0x100) ? 70000 : 300) + 1; slots[k] = malloc(sizes[k]); if (!slots[k]) { intact = 0; break; } for (unsigned i = 0; i < sizes[k]; i++) slots[k][i] = (unsigned char)(k ^ i); }
    }
    for (int k = 0; k < 512; k++) if (slots[k]) { for (unsigned i = 0; i < sizes[k]; i++) intact &= slots[k][i] == (unsigned char)(k ^ i); free(slots[k]); }
    CHECK(intact);

    /* setjmp / longjmp (the lifter turns these into host setjmp; Squirrel's compiler relies on it) */
    static int jb[16]; volatile int stage = 0; int r = _setjmp3(jb, 0);
    if (r == 0) { stage = 1; longjmp(jb, 7); stage = 99; }
    CHECKI(r, 7); CHECKI(stage, 1);
}

/* ---------------------------------------------------------------- files: stdio, low-level, Win32, case-insensitive paths */
static void test_stdio(void) {
    char b[256];
    _mkdir("ctest_dir"); _mkdir("CTest_Dir\\Sub");
    FILE *f = fopen("ctest_dir\\Text.TXT", "wb"); CHECK(f != 0); if (!f) return;
    fputs("line one\r\nline two\r\n", f); fprintf(f, "%d %s\r\n", 77, "x"); fclose(f);
    f = fopen("CTEST_DIR\\text.txt", "r"); CHECK(f != 0);                     /* other case, text mode: CRLF -> LF */
    if (f) { CHECKS(fgets(b, sizeof b, f), "line one\n"); CHECKS(fgets(b, sizeof b, f), "line two\n"); int n = 0; char w[8]; CHECKI(fscanf(f, "%d %s", &n, w), 2); CHECKI(n, 77); fclose(f); }
    f = fopen("ctest_dir/text.txt", "rb"); CHECK(f != 0);
    if (f) { CHECKI(fseek(f, 0, 2), 0); CHECKI(ftell(f), 26); fseek(f, 5, 0); CHECKI(fread(b, 1, 3, f), 3); b[3] = 0; CHECKS(b, "one"); CHECK(!feof(f)); fread(b, 1, 100, f); CHECK(feof(f)); fclose(f); }
    /* the engine's ZIP reader: size from _filelength(_fileno(f)), then seek to size - 22 for the end-of-directory record */
    f = fopen("ctest_dir/text.txt", "rb"); CHECK(f != 0);
    if (f) { long len = _filelength(_fileno(f)); CHECKI(len, 26); CHECKI(fseek(f, len - 22, 0), 0); CHECKI(fread(b, 1, 22, f), 22); CHECKI(ftell(f), 26); fclose(f); }
    /* buffered binary reads: seeks inside and outside what was read ahead, reads across the buffer, end of file */
    f = fopen("ctest_dir/text.txt", "rb"); CHECK(f != 0);
    if (f) { CHECKI(fread(b, 1, 4, f), 4); CHECK(!memcmp(b, "line", 4)); CHECKI(fseek(f, 2, 0), 0); CHECKI(fread(b, 1, 3, f), 3); CHECK(!memcmp(b, "ne ", 3)); CHECKI(ftell(f), 5);
             CHECKI(fseek(f, -3, 1), 0); CHECKI(ftell(f), 2); CHECKI(fread(b, 1, 2, f), 2); CHECK(!memcmp(b, "ne", 2)); CHECKI(fseek(f, 20, 0), 0); CHECKI(fread(b, 1, 2, f), 2); CHECK(!memcmp(b, "77", 2));
             CHECKI(fseek(f, 0, 0), 0); CHECKI(fread(b, 1, 26, f), 26); CHECK(!memcmp(b, "line one\r\nline two\r\n77 x\r\n", 26)); CHECK(!feof(f)); CHECKI(fread(b, 1, 1, f), 0); CHECK(feof(f)); fclose(f); }
    /* read then write on an update stream: the write lands where reading stopped, not after the read-ahead */
    f = fopen("ctest_dir/text.txt", "r+b"); CHECK(f != 0);
    if (f) { CHECKI(fread(b, 1, 2, f), 2); CHECKI(fseek(f, 0, 1), 0); CHECKI(fwrite("XY", 1, 2, f), 2); CHECKI(fseek(f, 0, 0), 0); CHECKI(fread(b, 1, 4, f), 4); CHECK(!memcmp(b, "liXY", 4));
             CHECKI(fseek(f, 2, 0), 0); fwrite("ne", 1, 2, f); fclose(f); }
    CHECK(fopen("ctest_dir\\missing.txt", "rb") == 0);
    struct _stat64i32 st; CHECKI(_stat64i32("ctest_dir\\TEXT.txt", &st), 0); CHECKI(st.st_size, 26); CHECK(st.st_mode & 0x8000);
    CHECKI(_stat64i32("ctest_dir\\sub", &st), 0); CHECK(st.st_mode & 0x4000); CHECKI(_stat64i32("nope", &st), -1);
    int fd = _open("ctest_dir\\bin.dat", 0x8000 | 0x100 | 0x200 | 1, 0600); CHECK(fd >= 0); CHECKI(_write(fd, "0123456789", 10), 10); _close(fd);
    fd = _open("ctest_dir\\BIN.dat", 0x8000); CHECK(fd >= 0); CHECKI(_filelength(fd), 10); CHECKI(_read(fd, b, 4), 4); CHECK(b[0] == '0' && b[3] == '3'); _close(fd);
    struct _finddata64i32_t fdd; int count = 0, saw = 0; long h = _findfirst64i32("ctest_dir\\*.txt", &fdd); CHECK(h != -1);
    if (h != -1) { do { count++; saw |= !_stricmp(fdd.name, "text.txt"); } while (_findnext64i32(h, &fdd) == 0); _findclose(h); } CHECKI(count, 1); CHECK(saw);
    CHECKI(rename("ctest_dir\\bin.dat", "ctest_dir\\bin2.dat"), 0); CHECKI(remove("ctest_dir\\BIN2.DAT"), 0); CHECKI(_stat64i32("ctest_dir\\bin2.dat", &st), -1);

    HANDLE hf = CreateFileA("ctest_dir\\w32.bin", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL); CHECK(hf != INVALID_HANDLE_VALUE);
    DWORD wr = 0; CHECK(WriteFile(hf, "abcdefgh", 8, &wr, NULL)); CHECKI(wr, 8); CloseHandle(hf);
    hf = CreateFileA("CTEST_DIR\\W32.BIN", GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL); CHECK(hf != INVALID_HANDLE_VALUE);
    CHECKI(SetFilePointer(hf, 0, NULL, FILE_END), 8); CHECKI(SetFilePointer(hf, 2, NULL, FILE_BEGIN), 2); CHECK(ReadFile(hf, b, 3, &wr, NULL)); CHECKI(wr, 3); CHECK(b[0] == 'c' && b[2] == 'e');
    CHECK(ReadFile(hf, b, 100, &wr, NULL)); CHECKI(wr, 3); CHECK(ReadFile(hf, b, 100, &wr, NULL)); CHECKI(wr, 0); CloseHandle(hf);
    CHECK(CreateFileA("ctest_dir\\none", GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL) == INVALID_HANDLE_VALUE); CHECKI(GetLastError(), ERROR_FILE_NOT_FOUND);
    CHECKI(GetFileAttributesA("ctest_dir\\sub"), FILE_ATTRIBUTE_DIRECTORY); CHECK(GetFileAttributesA("ctest_dir\\w32.bin") != INVALID_FILE_ATTRIBUTES && !(GetFileAttributesA("ctest_dir\\w32.bin") & FILE_ATTRIBUTE_DIRECTORY));
    WIN32_FIND_DATAA fdw; HANDLE fh = FindFirstFileA("ctest_dir\\*.bin", &fdw); CHECK(fh != INVALID_HANDLE_VALUE); if (fh != INVALID_HANDLE_VALUE) { CHECK(!_stricmp(fdw.cFileName, "w32.bin")); CHECKI(fdw.nFileSizeLow, 8); CHECK(!FindNextFileA(fh, &fdw)); FindClose(fh); }
    CHECK(DeleteFileA("ctest_dir\\w32.bin")); CHECK(!DeleteFileA("ctest_dir\\w32.bin"));
    f = fopen("ctest_dir\\cfg.ini", "wb"); fputs("[Video]\r\nwidth = 800\r\n; comment\r\nname=Dark Engine\r\n[Other]\r\nwidth=1\r\n", f); fclose(f);
    CHECKI(GetPrivateProfileIntA("video", "WIDTH", 5, ".\\ctest_dir\\cfg.ini"), 800); CHECKI(GetPrivateProfileIntA("Video", "height", 5, ".\\ctest_dir\\cfg.ini"), 5);
    CHECKI(GetPrivateProfileStringA("Video", "name", "def", b, sizeof b, ".\\ctest_dir\\cfg.ini"), 11); CHECKS(b, "Dark Engine");
    CHECKI(GetPrivateProfileStringA("Video", "none", "def", b, sizeof b, ".\\ctest_dir\\cfg.ini"), 3); CHECKS(b, "def");
    remove("ctest_dir\\cfg.ini"); remove("ctest_dir\\text.txt");
}

/* ---------------------------------------------------------------- Win32: memory, TLS, interlocked, strings, time */
static void test_win32(void) {
    HANDLE ph = GetProcessHeap(); CHECK(ph != 0);
    char *p = HeapAlloc(ph, HEAP_ZERO_MEMORY, 64); CHECK(p && p[63] == 0); CHECKI(HeapSize(ph, 0, p), 64); p[0] = 'k';
    p = HeapReAlloc(ph, 0, p, 100000); CHECK(p && p[0] == 'k'); CHECKI(HeapSize(ph, 0, p), 100000); CHECK(HeapFree(ph, 0, p));
    char *v = VirtualAlloc(NULL, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); CHECK(v != 0); CHECK(((DWORD)v & 0xFFFF) == 0);
    if (v) { v[0] = 1; v[(1 << 20) - 1] = 2; CHECK(v[(1 << 20) - 1] == 2); CHECK(VirtualFree(v, 0, MEM_RELEASE)); }
    DWORD t = TlsAlloc(); CHECK(t != TLS_OUT_OF_INDEXES); CHECK(TlsSetValue(t, (void *)0x1234)); CHECK(TlsGetValue(t) == (void *)0x1234); TlsFree(t);
    volatile LONG l = 5; CHECKI(InterlockedIncrement(&l), 6); CHECKI(InterlockedDecrement(&l), 5); CHECKI(InterlockedExchange(&l, 9), 5); CHECKI(InterlockedCompareExchange(&l, 1, 9), 9); CHECKI(l, 1); CHECKI(InterlockedCompareExchange(&l, 3, 9), 1); CHECKI(l, 1);
    WCHAR w[16]; char a[16]; CHECKI(MultiByteToWideChar(CP_ACP, 0, "abc", -1, w, 16), 4); CHECK(w[0] == 'a' && w[3] == 0); CHECKI(WideCharToMultiByte(CP_ACP, 0, w, -1, a, 16, NULL, NULL), 4); CHECKS(a, "abc");
    CHECKI(CompareStringA(LOCALE_USER_DEFAULT, NORM_IGNORECASE, "abc", -1, "ABC", -1), CSTR_EQUAL); CHECKI(CompareStringA(LOCALE_USER_DEFAULT, 0, "abc", -1, "abd", -1), CSTR_LESS_THAN);
    CHECKI(LCMapStringA(LOCALE_USER_DEFAULT, LCMAP_UPPERCASE, "mixEd", -1, a, 16), 6); CHECKS(a, "MIXED");
    DWORD t0 = GetTickCount(); LARGE_INTEGER q0, q1, qf; CHECK(QueryPerformanceFrequency(&qf)); CHECK(qf.QuadPart > 0); QueryPerformanceCounter(&q0);
    Sleep(30); DWORD t1 = GetTickCount(); QueryPerformanceCounter(&q1);
    CHECK(t1 - t0 >= 25 && t1 - t0 < 1000); double qms = (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart; CHECK(qms >= 25 && qms < 1000);
    CHECK(timeGetTime() - t1 < 1000);
    SYSTEMTIME st; GetLocalTime(&st); CHECK(st.wYear >= 2024 && st.wMonth >= 1 && st.wMonth <= 12);
    CRITICAL_SECTION cs; InitializeCriticalSection(&cs); EnterCriticalSection(&cs); EnterCriticalSection(&cs); LeaveCriticalSection(&cs); LeaveCriticalSection(&cs); DeleteCriticalSection(&cs); CHECK(1);
    HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL); CHECK(ev != 0); CHECKI(WaitForSingleObject(ev, 10), WAIT_TIMEOUT); SetEvent(ev); CHECKI(WaitForSingleObject(ev, 10), WAIT_OBJECT_0);
    ResetEvent(ev); CHECKI(WaitForSingleObject(ev, 0), WAIT_TIMEOUT); CloseHandle(ev);
    HANDLE ae = CreateEventA(NULL, FALSE, TRUE, NULL); CHECKI(WaitForSingleObject(ae, 0), WAIT_OBJECT_0); CHECKI(WaitForSingleObject(ae, 0), WAIT_TIMEOUT); CloseHandle(ae);   /* auto-reset */
    for (int i = 0; i < 3000; i++) { HANDLE e2 = CreateEventA(NULL, TRUE, FALSE, NULL); if (!e2) { CHECK(e2 != 0); break; } CloseHandle(e2); }   /* handle recycling */
    HANDLE hf = CreateFileA("ctest_handle.tmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); DWORD wr; CHECK(WriteFile(hf, "x", 1, &wr, NULL)); CloseHandle(hf); DeleteFileA("ctest_handle.tmp");
    HMODULE self = GetModuleHandleA(NULL); CHECK(self == (HMODULE)0x400000); char mp[MAX_PATH]; CHECK(GetModuleFileNameA(NULL, mp, MAX_PATH) > 0);
    CHECK(LoadLibraryA("no_such_library.dll") == 0);
}

/* ---------------------------------------------------------------- threads: _beginthreadex, CreateThread, critical sections, events, WINMM timers */
static volatile LONG shared_count; static CRITICAL_SECTION tcs; static int plain_count; static HANDLE go_ev;
static unsigned __stdcall worker(void *arg) {
    WaitForSingleObject(go_ev, INFINITE);
    for (int i = 0; i < 20000; i++) { InterlockedIncrement(&shared_count); EnterCriticalSection(&tcs); plain_count++; LeaveCriticalSection(&tcs); }
    return (unsigned)(DWORD)arg;
}
static DWORD WINAPI worker2(void *arg) { *(volatile int *)arg = 1234; return 7; }
static volatile LONG timer_hits;
static void CALLBACK on_timer(UINT id, UINT msg, DWORD_PTR user, DWORD_PTR r1, DWORD_PTR r2) { InterlockedIncrement((LONG *)user); }
static void test_threads(void) {
    InitializeCriticalSection(&tcs); go_ev = CreateEventA(NULL, TRUE, FALSE, NULL);
    unsigned tid = 0; HANDLE h1 = (HANDLE)_beginthreadex(NULL, 0, worker, (void *)11, 0, &tid), h2 = (HANDLE)_beginthreadex(NULL, 0, worker, (void *)22, 0, NULL);
    CHECK(h1 != 0); CHECK(h2 != 0); CHECK(tid != 0); if (!h1 || !h2) return;
    SetEvent(go_ev);
    for (int i = 0; i < 20000; i++) { InterlockedIncrement(&shared_count); EnterCriticalSection(&tcs); plain_count++; LeaveCriticalSection(&tcs); }
    CHECKI(WaitForSingleObject(h1, 10000), WAIT_OBJECT_0); CHECKI(WaitForSingleObject(h2, 10000), WAIT_OBJECT_0);
    DWORD code = 0; CHECK(GetExitCodeThread(h1, &code)); CHECKI(code, 11); GetExitCodeThread(h2, &code); CHECKI(code, 22); CloseHandle(h1); CloseHandle(h2);
    CHECKI(shared_count, 60000); CHECKI(plain_count, 60000);
    volatile int flag = 0; DWORD id; HANDLE h3 = CreateThread(NULL, 0, worker2, (void *)&flag, 0, &id); CHECK(h3 != 0);
    if (h3) { CHECKI(WaitForSingleObject(h3, 10000), WAIT_OBJECT_0); CHECKI(flag, 1234); GetExitCodeThread(h3, &code); CHECKI(code, 7); CloseHandle(h3); }
    timer_hits = 0; MMRESULT tm = timeSetEvent(10, 1, on_timer, (DWORD_PTR)&timer_hits, TIME_PERIODIC); CHECK(tm != 0);
    Sleep(200); timeKillEvent(tm); LONG hits = timer_hits; CHECK(hits >= 5 && hits <= 40); Sleep(50); CHECKI(timer_hits, hits);
    DeleteCriticalSection(&tcs); CloseHandle(go_ev);
}

/* ---------------------------------------------------------------- window: class, creation messages, message loop */
static int got_nccreate, got_create, got_activate, wnd_ok;
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) { got_nccreate++; wnd_ok = ((CREATESTRUCTA *)l)->cx == 320; }
    if (m == WM_CREATE) got_create++;
    if (m == WM_ACTIVATEAPP && w) got_activate++;
    return DefWindowProcA(h, m, w, l);
}
static HWND test_window(void) {
    WNDCLASSA wc = { 0 }; wc.lpfnWndProc = wndproc; wc.lpszClassName = "ctest"; wc.hInstance = GetModuleHandleA(NULL);
    CHECK(RegisterClassA(&wc) != 0);
    HWND h = CreateWindowExA(0, "ctest", "Conformance", WS_OVERLAPPEDWINDOW, 10, 10, 320, 240, NULL, NULL, wc.hInstance, NULL);
    CHECK(h != 0); CHECKI(got_nccreate, 1); CHECKI(got_create, 1); CHECK(wnd_ok);
    ShowWindow(h, SW_SHOW); SetForegroundWindow(h); CHECK(got_activate >= 1);
    RECT r; CHECK(GetClientRect(h, &r)); CHECK(r.right > 0 && r.bottom > 0);
    MSG msg; int n = 0; while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE) && n < 1000) { TranslateMessage(&msg); DispatchMessageA(&msg); n++; } CHECK(n < 1000);
    POINT pt; CHECK(GetCursorPos(&pt)); CHECK(GetSystemMetrics(SM_CXSCREEN) > 0);
    return h;
}

/* ---------------------------------------------------------------- Direct3D 9 through LoadLibrary/GetProcAddress, like the game */
typedef IDirect3D9 *(WINAPI *Create9)(UINT);
static void test_d3d9(HWND hwnd) {
    HMODULE m = LoadLibraryA("d3d9.dll"); CHECK(m != 0); if (!m) return;
    Create9 create = (Create9)GetProcAddress(m, "Direct3DCreate9"); CHECK(create != 0); if (!create) return;
    IDirect3D9 *d3d = create(D3D_SDK_VERSION); CHECK(d3d != 0); if (!d3d) return;
    CHECK(IDirect3D9_GetAdapterCount(d3d) >= 1);
    D3DCAPS9 caps; CHECKI(IDirect3D9_GetDeviceCaps(d3d, 0, D3DDEVTYPE_HAL, &caps), D3D_OK); CHECK(caps.MaxTextureWidth >= 256);
    D3DDISPLAYMODE dm; CHECKI(IDirect3D9_GetAdapterDisplayMode(d3d, 0, &dm), D3D_OK); CHECK(dm.Width > 0);
    UINT nm = IDirect3D9_GetAdapterModeCount(d3d, 0, D3DFMT_X8R8G8B8); CHECK(nm >= 1); CHECKI(IDirect3D9_EnumAdapterModes(d3d, 0, D3DFMT_X8R8G8B8, 0, &dm), D3D_OK);
    D3DPRESENT_PARAMETERS pp = { 0 }; pp.BackBufferWidth = 320; pp.BackBufferHeight = 240; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd; pp.Windowed = TRUE; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9 *dev = 0; CHECKI(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev), D3D_OK); CHECK(dev != 0); if (!dev) return;

    IDirect3DTexture9 *tex = 0; CHECKI(IDirect3DDevice9_CreateTexture(dev, 64, 32, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL), D3D_OK); CHECK(tex != 0);
    if (tex) {
        CHECKI(IDirect3DTexture9_GetLevelCount(tex), 7);
        D3DSURFACE_DESC sd; CHECKI(IDirect3DTexture9_GetLevelDesc(tex, 1, &sd), D3D_OK); CHECKI(sd.Width, 32); CHECKI(sd.Height, 16); CHECKI(sd.Format, D3DFMT_A8R8G8B8);
        D3DLOCKED_RECT lr; CHECKI(IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0), D3D_OK); CHECK(lr.Pitch >= 64 * 4 && lr.pBits);
        if (lr.pBits) for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x++) ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xFF00FF00u;
        CHECKI(IDirect3DTexture9_UnlockRect(tex, 0), D3D_OK);
        IDirect3DSurface9 *s1 = 0; CHECKI(IDirect3DTexture9_GetSurfaceLevel(tex, 0, &s1), D3D_OK); CHECK(s1 != 0);
        if (s1) { CHECKI(IDirect3DSurface9_GetDesc(s1, &sd), D3D_OK); CHECKI(sd.Width, 64); IDirect3DSurface9_Release(s1); }
    }
    IDirect3DTexture9 *t565 = 0; CHECKI(IDirect3DDevice9_CreateTexture(dev, 16, 16, 1, 0, D3DFMT_R5G6B5, D3DPOOL_MANAGED, &t565, NULL), D3D_OK);
    if (t565) { D3DLOCKED_RECT lr; IDirect3DTexture9_LockRect(t565, 0, &lr, NULL, 0); CHECK(lr.Pitch >= 32); IDirect3DTexture9_UnlockRect(t565, 0); }
    IDirect3DVertexBuffer9 *vb = 0; CHECKI(IDirect3DDevice9_CreateVertexBuffer(dev, 3 * 20, D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE, D3DPOOL_MANAGED, &vb, NULL), D3D_OK);
    struct V { float x, y, z, rhw; DWORD c; } tri[3] = { { 160, 20, 0.5f, 1, 0xFFFF0000u }, { 300, 220, 0.5f, 1, 0xFFFF0000u }, { 20, 220, 0.5f, 1, 0xFFFF0000u } };
    if (vb) { void *vp = 0; CHECKI(IDirect3DVertexBuffer9_Lock(vb, 0, 0, &vp, 0), D3D_OK); CHECK(vp != 0); if (vp) memcpy(vp, tri, sizeof tri); CHECKI(IDirect3DVertexBuffer9_Unlock(vb), D3D_OK);
              D3DVERTEXBUFFER_DESC vd; CHECKI(IDirect3DVertexBuffer9_GetDesc(vb, &vd), D3D_OK); CHECKI(vd.Size, 60); }
    IDirect3DIndexBuffer9 *ib = 0; CHECKI(IDirect3DDevice9_CreateIndexBuffer(dev, 6, D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_MANAGED, &ib, NULL), D3D_OK);
    if (ib) { WORD *ip = 0; IDirect3DIndexBuffer9_Lock(ib, 0, 0, (void **)&ip, 0); if (ip) { ip[0] = 0; ip[1] = 1; ip[2] = 2; } IDirect3DIndexBuffer9_Unlock(ib); }
    IDirect3DStateBlock9 *sb = 0; CHECKI(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb), D3D_OK);
    if (sb) { CHECKI(vcall(sb, 4, 0, 0, 0, 0, 0, 0, 0, 0), D3D_OK); CHECKI(vcall(sb, 5, 0, 0, 0, 0, 0, 0, 0, 0), D3D_OK); IDirect3DStateBlock9_Release(sb); }   /* Capture, Apply */
    CHECKI(IDirect3DDevice9_BeginStateBlock(dev), D3D_OK); IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE); sb = 0; CHECKI(IDirect3DDevice9_EndStateBlock(dev, &sb), D3D_OK); if (sb) { vcall(sb, 5, 0, 0, 0, 0, 0, 0, 0, 0); IDirect3DStateBlock9_Release(sb); }

    for (int frame = 0; frame < 3; frame++) {
        CHECKI(IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000040u, 1.0f, 0), D3D_OK);
        CHECKI(IDirect3DDevice9_BeginScene(dev), D3D_OK);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE);
        IDirect3DDevice9_SetTexture(dev, 0, NULL); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        CHECKI(IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE), D3D_OK);
        CHECKI(IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof tri[0]), D3D_OK);
        if (vb) { IDirect3DDevice9_SetStreamSource(dev, 0, vb, 0, 20); CHECKI(IDirect3DDevice9_DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1), D3D_OK); }
        if (vb && ib) { IDirect3DDevice9_SetIndices(dev, ib); CHECKI(IDirect3DDevice9_DrawIndexedPrimitive(dev, D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1), D3D_OK); }
        WORD idx[3] = { 0, 1, 2 }; CHECKI(IDirect3DDevice9_DrawIndexedPrimitiveUP(dev, D3DPT_TRIANGLELIST, 0, 3, 1, idx, D3DFMT_INDEX16, tri, sizeof tri[0]), D3D_OK);
        if (tex) { IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex); IDirect3DDevice9_SetTexture(dev, 0, NULL); }
        CHECKI(IDirect3DDevice9_EndScene(dev), D3D_OK);
        CHECKI(IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL), D3D_OK);
    }
    /* every IDirect3DDevice9 method that is safe to call with zero arguments, through its vtable slot, for the stack cleanup */
    static const struct { int slot, n; } dev_slots[] = { { 3, 0 }, { 4, 0 }, { 5, 0 }, { 15, 0 }, { 41, 0 }, { 42, 0 }, { 57, 2 }, { 67, 3 }, { 69, 3 }, { 79, 1 }, { 81, 3 }, { 89, 1 }, { 104, 1 }, { 107, 1 } };
    for (unsigned i = 0; i < sizeof dev_slots / sizeof *dev_slots; i++) vcall(dev, dev_slots[i].slot, dev_slots[i].n, 0, 0, 0, 0, 0, 0, 0);
    vcall(dev, 85, 6, 0, 0, 0, 0, 0, 0, 0);       /* ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags) */
    if (tex) IDirect3DTexture9_Release(tex); if (t565) IDirect3DTexture9_Release(t565); if (vb) IDirect3DVertexBuffer9_Release(vb); if (ib) IDirect3DIndexBuffer9_Release(ib);
    IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
}

/* ---------------------------------------------------------------- pixels: what a real renderer draws (skipped unless -pixels; the null backend draws nothing) */
typedef struct { float x, y, z, rhw; DWORD c, s; float u, v; } TV;
static DWORD px(IDirect3DDevice9 *dev, IDirect3DSurface9 *bb, IDirect3DSurface9 *sys, int x, int y) {
    if (IDirect3DDevice9_GetRenderTargetData(dev, bb, sys) != D3D_OK) return 0xDEADBEEF;
    D3DLOCKED_RECT lr; if (IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY) != D3D_OK) return 0xDEADBEEF;
    DWORD v = ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] & 0xFFFFFF; IDirect3DSurface9_UnlockRect(sys); return v;
}
static int near_rgb(DWORD a, DWORD b) { for (int i = 0; i < 24; i += 8) { int d = (int)((a >> i) & 255) - (int)((b >> i) & 255); if (d > 3 || d < -3) return 0; } return 1; }
#define CHECKPX(x, y, want) do { DWORD g_ = px(dev, bb, sys, x, y); check(near_rgb(g_, want), "pixel (" #x ", " #y ") == " #want, __LINE__); if (!near_rgb(g_, want)) { char t_[120]; sprintf(t_, "     got %06lx\n", (unsigned long)g_); out(t_); } } while (0)
static void rect(IDirect3DDevice9 *dev, float x0, float y0, float x1, float y1, float z, DWORD c, DWORD s) {
    TV q[4] = { { x0, y0, z, 1, c, s, 0, 0 }, { x1, y0, z, 1, c, s, 1, 0 }, { x0, y1, z, 1, c, s, 0, 1 }, { x1, y1, z, 1, c, s, 1, 1 } };
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1); IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof q[0]);
}
static void test_pixels(HWND hwnd) {
    HMODULE m = LoadLibraryA("d3d9.dll"); Create9 create = (Create9)GetProcAddress(m, "Direct3DCreate9"); IDirect3D9 *d3d = create(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp = { 0 }; pp.BackBufferWidth = 64; pp.BackBufferHeight = 64; pp.BackBufferFormat = D3DFMT_X8R8G8B8; pp.BackBufferCount = 1; pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = hwnd; pp.Windowed = TRUE; pp.EnableAutoDepthStencil = TRUE; pp.AutoDepthStencilFormat = D3DFMT_D24S8;
    IDirect3DDevice9 *dev = 0; CHECKI(IDirect3D9_CreateDevice(d3d, 0, D3DDEVTYPE_HAL, hwnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev), D3D_OK); if (!dev) return;
    IDirect3DSurface9 *bb = 0, *sys = 0; IDirect3DDevice9_GetBackBuffer(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    CHECKI(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL), D3D_OK); if (!bb || !sys) return;
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE); IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF0000FFu, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    rect(dev, 10, 10, 20, 20, 0.5f, 0xFFFFFFFFu, 0xFF000000u);                        /* exact pixel edges: Direct3D 9 pixel centres are at integers */
    IDirect3DDevice9_EndScene(dev);
    CHECKPX(10, 10, 0xFFFFFF); CHECKPX(19, 19, 0xFFFFFF); CHECKPX(9, 10, 0x0000FF); CHECKPX(20, 10, 0x0000FF); CHECKPX(10, 20, 0x0000FF); CHECKPX(10, 9, 0x0000FF); CHECKPX(0, 0, 0x0000FF);
    /* texture, modulated with the vertex colour */
    IDirect3DTexture9 *tex = 0; IDirect3DDevice9_CreateTexture(dev, 4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, NULL);
    D3DLOCKED_RECT lr; IDirect3DTexture9_LockRect(tex, 0, &lr, NULL, 0); for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = x < 2 ? 0xFF00FF00u : 0xFFFF00FFu; IDirect3DTexture9_UnlockRect(tex, 0);
    IDirect3DDevice9_SetTexture(dev, 0, (IDirect3DBaseTexture9 *)tex); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_MODULATE); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    IDirect3DDevice9_BeginScene(dev); rect(dev, 30, 30, 38, 38, 0.5f, 0xFFFFFFFFu, 0xFF000000u); IDirect3DDevice9_EndScene(dev);
    CHECKPX(31, 33, 0x00FF00); CHECKPX(36, 33, 0xFF00FF);                               /* left half green, right half magenta: v = 0 is the top row */
    IDirect3DDevice9_BeginScene(dev); rect(dev, 30, 40, 38, 48, 0.5f, 0xFF808080u, 0xFF000000u); IDirect3DDevice9_EndScene(dev);
    CHECKPX(31, 44, 0x008000);
    IDirect3DDevice9_SetTexture(dev, 0, NULL); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1); IDirect3DDevice9_SetTextureStageState(dev, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    /* alpha blending */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE); IDirect3DDevice9_SetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_SRCALPHA); IDirect3DDevice9_SetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    IDirect3DDevice9_BeginScene(dev); rect(dev, 40, 10, 50, 20, 0.5f, 0x80FF0000u, 0xFF000000u); IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    CHECKPX(45, 15, 0x80007F);
    /* alpha test */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, TRUE); IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAFUNC, D3DCMP_GREATEREQUAL); IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHAREF, 0x80);
    IDirect3DDevice9_BeginScene(dev); rect(dev, 50, 30, 54, 34, 0.5f, 0x7FFFFF00u, 0xFF000000u); rect(dev, 54, 30, 58, 34, 0.5f, 0x80FFFF00u, 0xFF000000u); IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
    CHECKPX(51, 31, 0x0000FF); CHECKPX(55, 31, 0xFFFF00);
    /* depth: the nearer red quad stays in front of the farther green one drawn later */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, TRUE); IDirect3DDevice9_SetRenderState(dev, D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    IDirect3DDevice9_BeginScene(dev); rect(dev, 2, 40, 12, 50, 0.3f, 0xFFFF0000u, 0xFF000000u); rect(dev, 2, 40, 12, 50, 0.6f, 0xFF00FF00u, 0xFF000000u); IDirect3DDevice9_EndScene(dev);
    CHECKPX(6, 45, 0xFF0000);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);
    /* fog from the specular alpha (vertex fog mode none): alpha 0 is fully fogged */
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, TRUE); IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGCOLOR, 0xFFFFFF00u);
    IDirect3DDevice9_BeginScene(dev); rect(dev, 14, 40, 20, 46, 0.5f, 0xFF0000FFu, 0x00000000u); rect(dev, 20, 40, 26, 46, 0.5f, 0xFFFF0000u, 0xFF000000u); IDirect3DDevice9_EndScene(dev);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_FOGENABLE, FALSE);
    CHECKPX(16, 42, 0xFFFF00); CHECKPX(22, 42, 0xFF0000);
    /* culling: this triangle is counter-clockwise on screen, so D3DCULL_CCW removes it and D3DCULL_CW keeps it */
    TV tri[3] = { { 56, 40, 0.5f, 1, 0xFF00FF00u, 0xFF000000u, 0, 0 }, { 56, 50, 0.5f, 1, 0xFF00FF00u, 0xFF000000u, 0, 0 }, { 63, 40, 0.5f, 1, 0xFF00FF00u, 0xFF000000u, 0, 0 } };
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_CCW);
    IDirect3DDevice9_BeginScene(dev); IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1); IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof tri[0]); IDirect3DDevice9_EndScene(dev);
    CHECKPX(57, 41, 0x0000FF);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_CW);
    IDirect3DDevice9_BeginScene(dev); IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, tri, sizeof tri[0]); IDirect3DDevice9_EndScene(dev);
    CHECKPX(57, 41, 0x00FF00);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    /* transformed geometry: identity world/view, an orthographic projection that maps x, y in [-1, 1] to the viewport */
    struct { float x, y, z; DWORD c; } cv[3] = { { -1.0f, 1.0f, 0.5f, 0xFFFF8000u }, { 0.0f, 1.0f, 0.5f, 0xFFFF8000u }, { -1.0f, 0.0f, 0.5f, 0xFFFF8000u } };
    D3DMATRIX id = { { { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 } } };
    IDirect3DDevice9_SetTransform(dev, D3DTS_WORLD, &id); IDirect3DDevice9_SetTransform(dev, D3DTS_VIEW, &id); IDirect3DDevice9_SetTransform(dev, D3DTS_PROJECTION, &id);
    IDirect3DDevice9_BeginScene(dev); IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZ | D3DFVF_DIFFUSE); IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, cv, sizeof cv[0]); IDirect3DDevice9_EndScene(dev);
    CHECKPX(2, 2, 0xFF8000); CHECKPX(40, 2, 0x0000FF);                                     /* top-left quarter of the screen, upper-left half */
    /* a viewport: Clear only touches its area */
    D3DVIEWPORT9 vp = { 60, 60, 4, 4, 0, 1 }; IDirect3DDevice9_SetViewport(dev, &vp); IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xFF00FFFFu, 1.0f, 0);
    CHECKPX(61, 61, 0x00FFFF); CHECKPX(59, 61, 0x0000FF);
    /* StretchRect from an offscreen surface into the back buffer */
    IDirect3DSurface9 *off = 0; IDirect3DDevice9_CreateOffscreenPlainSurface(dev, 2, 2, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &off, NULL);
    if (off) { IDirect3DSurface9_LockRect(off, &lr, NULL, 0); for (int y = 0; y < 2; y++) for (int x = 0; x < 2; x++) ((DWORD *)((char *)lr.pBits + y * lr.Pitch))[x] = 0xFF123456u; IDirect3DSurface9_UnlockRect(off);
               RECT dr = { 0, 56, 8, 64 }; CHECKI(IDirect3DDevice9_StretchRect(dev, off, NULL, bb, &dr, D3DTEXF_POINT), D3D_OK); CHECKPX(4, 60, 0x123456); CHECKPX(9, 60, 0x0000FF); IDirect3DSurface9_Release(off); }
    IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
    IDirect3DTexture9_Release(tex); IDirect3DSurface9_Release(sys); IDirect3DSurface9_Release(bb); IDirect3DDevice9_Release(dev); IDirect3D9_Release(d3d);
}

/* ---------------------------------------------------------------- DirectSound */
typedef HRESULT (WINAPI *DSCreate)(LPCGUID, LPDIRECTSOUND *, LPUNKNOWN);
static const GUID iid_listener = { 0x279AFA84, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static const GUID iid_3dbuffer = { 0x279AFA86, 0x4981, 0x11CE, { 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 } };
static void test_dsound(HWND hwnd) {
    HMODULE m = LoadLibraryA("dsound.dll"); CHECK(m != 0); if (!m) return;
    DSCreate create = (DSCreate)GetProcAddress(m, "DirectSoundCreate"); CHECK(create != 0); if (!create) return;
    LPDIRECTSOUND ds = 0; CHECKI(create(NULL, &ds, NULL), DS_OK); CHECK(ds != 0); if (!ds) return;
    CHECKI(IDirectSound_SetCooperativeLevel(ds, hwnd, DSSCL_PRIORITY), DS_OK);
    DSBUFFERDESC pd = { sizeof pd }; pd.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D; LPDIRECTSOUNDBUFFER prim = 0;
    CHECKI(IDirectSound_CreateSoundBuffer(ds, &pd, &prim, NULL), DS_OK); CHECK(prim != 0);
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0 };
    if (prim) {
        CHECKI(IDirectSoundBuffer_SetFormat(prim, &wf), DS_OK);
        LPDIRECTSOUND3DLISTENER lis = 0; CHECKI(IDirectSoundBuffer_QueryInterface(prim, &iid_listener, (void **)&lis), DS_OK); CHECK(lis != 0);
        if (lis) {
            float one = 1.0f; unsigned f1; memcpy(&f1, &one, 4);
            CHECKI(vcall(lis, 14, 4, f1, f1, f1, DS3D_DEFERRED, 0, 0, 0), DS_OK);               /* SetPosition(x, y, z, apply) */
            CHECKI(vcall(lis, 16, 4, f1, 0, 0, DS3D_DEFERRED, 0, 0, 0), DS_OK);                 /* SetVelocity */
            CHECKI(vcall(lis, 13, 7, 0, 0, f1, 0, f1, 0, DS3D_DEFERRED), DS_OK);                /* SetOrientation */
            CHECKI(vcall(lis, 17, 0, 0, 0, 0, 0, 0, 0, 0), DS_OK);                              /* CommitDeferredSettings */
            D3DVECTOR pos = { 0 }; CHECKI(IDirectSound3DListener_GetPosition(lis, &pos), DS_OK);
            IDirectSound3DListener_Release(lis);
        }
    }
    DSBUFFERDESC bd = { sizeof bd }; bd.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_CTRL3D; bd.dwBufferBytes = 22050 * 4; bd.lpwfxFormat = &wf;
    LPDIRECTSOUNDBUFFER buf = 0; CHECKI(IDirectSound_CreateSoundBuffer(ds, &bd, &buf, NULL), DS_OK); CHECK(buf != 0); if (!buf) return;
    void *p1, *p2; DWORD n1, n2; CHECKI(IDirectSoundBuffer_Lock(buf, 0, 22050 * 4, &p1, &n1, &p2, &n2, 0), DS_OK); CHECKI(n1, 22050 * 4); CHECK(p2 == 0 || n2 == 0);
    if (p1) { short *s = p1; for (unsigned i = 0; i < n1 / 2; i++) s[i] = (short)((i * 300) & 0x3FFF); }
    CHECKI(IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2), DS_OK);
    CHECKI(IDirectSoundBuffer_Lock(buf, 22050 * 4 - 100, 300, &p1, &n1, &p2, &n2, 0), DS_OK); CHECKI(n1, 100); CHECKI(n2, 200); CHECK(p2 != 0); IDirectSoundBuffer_Unlock(buf, p1, n1, p2, n2);
    WAVEFORMATEX got; DWORD gsz = 0; CHECKI(IDirectSoundBuffer_GetFormat(buf, &got, sizeof got, &gsz), DS_OK); CHECKI(got.nSamplesPerSec, 22050); CHECKI(got.nChannels, 2);
    CHECKI(IDirectSoundBuffer_SetVolume(buf, -600), DS_OK); LONG vol = 0; IDirectSoundBuffer_GetVolume(buf, &vol); CHECKI(vol, -600);
    CHECKI(IDirectSoundBuffer_SetFrequency(buf, 22050), DS_OK); DWORD fr = 0; IDirectSoundBuffer_GetFrequency(buf, &fr); CHECKI(fr, 22050);
    LPDIRECTSOUND3DBUFFER b3 = 0; CHECKI(IDirectSoundBuffer_QueryInterface(buf, &iid_3dbuffer, (void **)&b3), DS_OK);
    if (b3) { float two = 2.0f; unsigned f2; memcpy(&f2, &two, 4); CHECKI(vcall(b3, 20, 4, f2, 0, 0, DS3D_IMMEDIATE, 0, 0, 0), DS_OK); CHECKI(vcall(b3, 17, 2, f2, DS3D_IMMEDIATE, 0, 0, 0, 0, 0), DS_OK); IDirectSound3DBuffer_Release(b3); }
    DWORD st = 0; IDirectSoundBuffer_GetStatus(buf, &st); CHECK(!(st & DSBSTATUS_PLAYING));
    CHECKI(IDirectSoundBuffer_SetCurrentPosition(buf, 4000), DS_OK); DWORD pc = 0, wc = 0; IDirectSoundBuffer_GetCurrentPosition(buf, &pc, &wc); CHECKI(pc, 4000);
    CHECKI(IDirectSoundBuffer_Play(buf, 0, 0, DSBPLAY_LOOPING), DS_OK); IDirectSoundBuffer_GetStatus(buf, &st); CHECK((st & DSBSTATUS_PLAYING) && (st & DSBSTATUS_LOOPING));
    Sleep(150); IDirectSoundBuffer_GetCurrentPosition(buf, &pc, &wc); CHECK(pc < 22050u * 4); CHECK(pc != 4000); CHECK(wc < 22050u * 4);
    CHECKI(IDirectSoundBuffer_Stop(buf), DS_OK); DWORD stopped = 0; IDirectSoundBuffer_GetCurrentPosition(buf, &stopped, &wc); CHECK(stopped < 22050u * 4);
    Sleep(30); DWORD again = 0; IDirectSoundBuffer_GetCurrentPosition(buf, &again, &wc); CHECKI(again, stopped);      /* a stopped buffer does not move */
    LPDIRECTSOUNDBUFFER dup = 0; CHECKI(IDirectSound_DuplicateSoundBuffer(ds, buf, &dup), DS_OK); if (dup) { CHECKI(IDirectSoundBuffer_Play(dup, 0, 0, 0), DS_OK); IDirectSoundBuffer_Stop(dup); IDirectSoundBuffer_Release(dup); }
    DSBUFFERDESC sd = { sizeof sd }; sd.dwBufferBytes = 4000; sd.lpwfxFormat = &wf; LPDIRECTSOUNDBUFFER one = 0; IDirectSound_CreateSoundBuffer(ds, &sd, &one, NULL);
    if (one) { IDirectSoundBuffer_Play(one, 0, 0, 0); Sleep(150); IDirectSoundBuffer_GetStatus(one, &st); CHECK(!(st & DSBSTATUS_PLAYING)); IDirectSoundBuffer_Release(one); }   /* 45 ms of sound: done */
    IDirectSoundBuffer_Release(buf); if (prim) IDirectSoundBuffer_Release(prim); IDirectSound_Release(ds);
}

/* ---------------------------------------------------------------- DirectInput (version 7, as the engine uses it) */
typedef HRESULT (WINAPI *DICreate)(HINSTANCE, DWORD, LPDIRECTINPUTA *, LPUNKNOWN);
static const GUID guid_kbd = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static const GUID guid_mouse = { 0x6F1D2B60, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
static void test_dinput(HWND hwnd) {
    HMODULE m = LoadLibraryA("dinput.dll"); CHECK(m != 0); if (!m) return;
    DICreate create = (DICreate)GetProcAddress(m, "DirectInputCreateA"); CHECK(create != 0); if (!create) return;
    LPDIRECTINPUTA di = 0; CHECKI(create(GetModuleHandleA(NULL), 0x0700, &di, NULL), DI_OK); CHECK(di != 0); if (!di) return;
    LPDIRECTINPUTDEVICEA kb = 0; CHECKI(IDirectInput_CreateDevice(di, &guid_kbd, &kb, NULL), DI_OK); CHECK(kb != 0);
    if (kb) {
        static DIOBJECTDATAFORMAT objs[256]; DIDATAFORMAT df = { sizeof df, sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS, 256, 256, objs };
        for (int i = 0; i < 256; i++) { objs[i].pguid = 0; objs[i].dwOfs = i; objs[i].dwType = 0x80000000u | DIDFT_BUTTON | DIDFT_MAKEINSTANCE(i); objs[i].dwFlags = 0; }
        CHECKI(IDirectInputDevice_SetDataFormat(kb, &df), DI_OK); CHECKI(IDirectInputDevice_SetCooperativeLevel(kb, hwnd, DISCL_NONEXCLUSIVE | DISCL_FOREGROUND), DI_OK);
        CHECK(SUCCEEDED(IDirectInputDevice_Acquire(kb)));
        BYTE keys[256]; memset(keys, 0x55, sizeof keys); CHECKI(IDirectInputDevice_GetDeviceState(kb, sizeof keys, keys), DI_OK); int clear = 1; for (int i = 0; i < 256; i++) clear &= keys[i] == 0; CHECK(clear);
        DIPROPDWORD pd = { { sizeof pd, sizeof pd.diph, 0, DIPH_DEVICE }, 32 }; CHECKI(IDirectInputDevice_SetProperty(kb, DIPROP_BUFFERSIZE, &pd.diph), DI_OK);
        DIDEVICEOBJECTDATA od[8]; DWORD n = 8; CHECKI(IDirectInputDevice_GetDeviceData(kb, sizeof od[0], od, &n, 0), DI_OK); CHECKI(n, 0);
        IDirectInputDevice_Unacquire(kb); IDirectInputDevice_Release(kb);
    }
    LPDIRECTINPUTDEVICEA ms = 0; CHECKI(IDirectInput_CreateDevice(di, &guid_mouse, &ms, NULL), DI_OK);
    if (ms) {
        DIMOUSESTATE mst; CHECK(IDirectInputDevice_Acquire(ms) != DI_OK);                /* no data format yet */
        static const GUID gx = { 0xA36D02E0, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } }, gy = { 0xA36D02E1, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } }, gz = { 0xA36D02E2, 0xC9F3, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
        static DIOBJECTDATAFORMAT mo[7]; mo[0] = (DIOBJECTDATAFORMAT){ &gx, 0, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 }; mo[1] = (DIOBJECTDATAFORMAT){ &gy, 4, DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 };
        mo[2] = (DIOBJECTDATAFORMAT){ &gz, 8, 0x80000000u | DIDFT_AXIS | DIDFT_ANYINSTANCE, 0 }; for (int b = 0; b < 4; b++) mo[3 + b] = (DIOBJECTDATAFORMAT){ 0, 12 + (DWORD)b, 0x80000000u | DIDFT_BUTTON | DIDFT_ANYINSTANCE, 0 };
        DIDATAFORMAT mdf = { sizeof mdf, sizeof(DIOBJECTDATAFORMAT), DIDF_RELAXIS, sizeof(DIMOUSESTATE), 7, mo };
        CHECKI(IDirectInputDevice_SetDataFormat(ms, &mdf), DI_OK); CHECK(SUCCEEDED(IDirectInputDevice_Acquire(ms)));
        memset(&mst, 0x55, sizeof mst); CHECKI(IDirectInputDevice_GetDeviceState(ms, sizeof mst, &mst), DI_OK); CHECK(mst.lX == 0 && mst.lY == 0 && mst.rgbButtons[0] == 0);
        IDirectInputDevice_Release(ms);
    }
    CHECKI(vcall(di, 7, 2, (unsigned)GetModuleHandleA(NULL), 0x0700, 0, 0, 0, 0, 0), DI_OK);       /* IDirectInput::Initialize(hinst, version) */
    IDirectInput_Release(di);
}

/* ---------------------------------------------------------------- entry */
void __stdcall start(void) {
    const char *cl = GetCommandLineA(); char buf[256]; strncpy(buf, cl, 255); buf[255] = 0;
    skip_mask = 1 << 8;                       /* "pixels" runs only when asked for */
    for (char *t = strtok(buf, " "); t; t = strtok(0, " ")) {
        if (seq(t, "-skip")) { char *w = strtok(0, " "); for (int i = 0; w && names[i]; i++) if (!_stricmp(w, names[i])) skip_mask |= 1 << i; }
        if (seq(t, "-pixels")) skip_mask &= ~(1 << 8);
    }
    if (on(0)) test_crt();
    if (on(1)) test_stdio();
    if (on(2)) test_win32();
    if (on(3)) test_threads();
    HWND h = 0; if (on(4)) h = test_window();
    if (on(5)) test_d3d9(h);
    if (on(6)) test_dsound(h);
    if (on(7)) test_dinput(h);
    if (on(8)) test_pixels(h);
    char b[128]; sprintf(b, "conformance: %d of %d checks failed\n", fails, checks); out(b);
    ExitProcess((UINT)fails);
}
