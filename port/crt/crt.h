/* crt.h: shared between the C runtime files. */
#ifndef CRT_H
#define CRT_H
#include "core.h"
typedef struct Stream Stream;
int  stream_getc(Stream *s);                 /* text-mode translation applied; -1 at end */
void stream_ungetc(Stream *s, int ch);
int  crt_format(char **out, const char *fmt, uint32_t va, int wide_default);   /* returns length; free(*out) */
int  crt_sscanf(const char *s, const char *fmt, uint32_t va);
int  crt_fscanf(Stream *st, const char *fmt, uint32_t va);
Stream *crt_stream(uint32_t guest_file);     /* NULL if not one of ours */
void crt_set_errno(int e);                   /* MSVC errno values: ENOENT 2, EBADF 9, EACCES 13, EEXIST 17, EINVAL 22, ERANGE 34 */
int  crt_errno_of(int plat_err);
uint32_t crt_teb_slot(int which);            /* per-thread CRT state kept in the thread's TEB: 0 errno, 1 rand seed, 2 strtok */
#endif
