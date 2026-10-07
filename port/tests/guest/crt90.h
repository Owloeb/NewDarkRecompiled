/* crt90.h: the MSVCR90 functions the conformance test imports, declared by hand (the test is built freestanding, so no
 * C library headers: every call below goes through the import table to the portable host, exactly like the game's). */
#ifndef CRT90_H
#define CRT90_H
typedef unsigned int size_t;
typedef char *va_list;
#define IMP __declspec(dllimport)

IMP int __cdecl sprintf(char *, const char *, ...);
IMP int __cdecl _snprintf(char *, size_t, const char *, ...);
IMP int __cdecl _vsnprintf(char *, size_t, const char *, va_list);
IMP int __cdecl sscanf(const char *, const char *, ...);
IMP void *__cdecl malloc(size_t);
IMP void *__cdecl calloc(size_t, size_t);
IMP void *__cdecl realloc(void *, size_t);
IMP void __cdecl free(void *);
IMP size_t __cdecl _msize(void *);
IMP void *__cdecl memcpy(void *, const void *, size_t);
IMP void *__cdecl memmove(void *, const void *, size_t);
IMP void *__cdecl memset(void *, int, size_t);
IMP int __cdecl memcmp(const void *, const void *, size_t);
IMP void *__cdecl memchr(const void *, int, size_t);
IMP char *__cdecl strchr(const char *, int);
IMP char *__cdecl strrchr(const char *, int);
IMP char *__cdecl strstr(const char *, const char *);
IMP char *__cdecl strncpy(char *, const char *, size_t);
IMP int __cdecl strncmp(const char *, const char *, size_t);
IMP int __cdecl _stricmp(const char *, const char *);
IMP int __cdecl _strnicmp(const char *, const char *, size_t);
IMP char *__cdecl _strdup(const char *);
IMP char *__cdecl _strlwr(char *);
IMP char *__cdecl strtok(char *, const char *);
IMP long __cdecl strtol(const char *, char **, int);
IMP unsigned long __cdecl strtoul(const char *, char **, int);
IMP int __cdecl atoi(const char *);
IMP double __cdecl atof(const char *);
IMP char *__cdecl _itoa(int, char *, int);
IMP int __cdecl toupper(int);
IMP int __cdecl isdigit(int);
IMP void __cdecl qsort(void *, size_t, size_t, int (__cdecl *)(const void *, const void *));
IMP void *__cdecl bsearch(const void *, const void *, size_t, size_t, int (__cdecl *)(const void *, const void *));
IMP int __cdecl rand(void);
IMP void __cdecl srand(unsigned);
IMP double __cdecl floor(double);
IMP double __cdecl ceil(double);
IMP double __cdecl ldexp(double, int);

typedef struct { int _p; } FILE;
IMP FILE *__cdecl fopen(const char *, const char *);
IMP int __cdecl fclose(FILE *);
IMP size_t __cdecl fread(void *, size_t, size_t, FILE *);
IMP size_t __cdecl fwrite(const void *, size_t, size_t, FILE *);
IMP int __cdecl fseek(FILE *, long, int);
IMP long __cdecl ftell(FILE *);
IMP char *__cdecl fgets(char *, int, FILE *);
IMP int __cdecl fputs(const char *, FILE *);
IMP int __cdecl fprintf(FILE *, const char *, ...);
IMP int __cdecl feof(FILE *);
IMP int __cdecl fscanf(FILE *, const char *, ...);
IMP int __cdecl _open(const char *, int, ...);
IMP int __cdecl _read(int, void *, unsigned);
IMP int __cdecl _write(int, const void *, unsigned);
IMP int __cdecl _close(int);
IMP long __cdecl _filelength(int);
IMP int __cdecl _fileno(FILE *);
IMP int __cdecl remove(const char *);
IMP int __cdecl rename(const char *, const char *);
IMP int __cdecl _mkdir(const char *);
struct _stat64i32 { unsigned st_dev; unsigned short st_ino, st_mode; short st_nlink, st_uid, st_gid; unsigned st_rdev; long st_size; long long st_atime_, st_mtime_, st_ctime_; };
IMP int __cdecl _stat64i32(const char *, struct _stat64i32 *);
struct _finddata64i32_t { unsigned attrib; long long time_create, time_access, time_write; unsigned size; char name[260]; };
IMP long __cdecl _findfirst64i32(const char *, struct _finddata64i32_t *);
IMP int __cdecl _findnext64i32(long, struct _finddata64i32_t *);
IMP int __cdecl _findclose(long);
IMP long long __cdecl _time64(long long *);
IMP unsigned __cdecl _beginthreadex(void *, unsigned, unsigned (__stdcall *)(void *), void *, unsigned, unsigned *);
IMP int __cdecl _setjmp3(void *, int, ...);
IMP void __cdecl longjmp(void *, int);
/* the x87 math helpers: argument(s) in st(0)/st(1), result in st(0); called through the asm helpers in conformance.c */
IMP void __cdecl _CIpow(void);
IMP void __cdecl _CIsqrt(void);
IMP void __cdecl _CIatan2(void);
IMP void __cdecl _CIfmod(void);
#endif
