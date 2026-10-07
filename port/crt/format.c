/* format.c: printf and scanf with the Microsoft C runtime's (MSVCR90) rules, reading arguments from guest memory.
 *
 * Differences from the C library of most hosts that matter to a game: three-digit exponents (1.5e+000), "1.#INF00" /
 * "-1.#IND00" for infinities and NaNs, %p as eight upper-case hex digits, %I64 / %I32 size prefixes, %S for wide
 * strings, and _snprintf's truncation rules (handled by the callers in msvcrt.c). The host's snprintf formats single
 * numbers; everything else (padding, flags, exponents) is done here so the result does not depend on the host. */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include "core.h"
#include "crt.h"

typedef struct { char *s; size_t n, cap; } Buf;
static void put(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { while (b->n + n + 1 > b->cap) b->cap = b->cap ? b->cap * 2 : 256; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void putc_n(Buf *b, char ch, int n) { char t[64]; memset(t, ch, sizeof t); while (n > 0) { int k = n > 64 ? 64 : n; put(b, t, (size_t)k); n -= k; } }
/* body with sign/prefix already in it: applies width, '-' and '0' (zero padding goes after the sign and 0x prefix) */
static void pad(Buf *b, const char *body, int width, int left, int zero) {
    int len = (int)strlen(body);
    if (len >= width) { put(b, body, (size_t)len); return; }
    if (left) { put(b, body, (size_t)len); putc_n(b, ' ', width - len); return; }
    if (!zero) { putc_n(b, ' ', width - len); put(b, body, (size_t)len); return; }
    int pre = 0; if (body[0] == '-' || body[0] == '+' || body[0] == ' ') pre = 1;
    if ((body[pre] == '0') && (body[pre + 1] == 'x' || body[pre + 1] == 'X')) pre += 2;
    put(b, body, (size_t)pre); putc_n(b, '0', width - len); put(b, body + pre, (size_t)(len - pre));
}
static void fix_exponent(char *s) {          /* e+05 -> e+005 */
    char *e = strpbrk(s, "eE"); if (!e || (e[1] != '+' && e[1] != '-')) return;
    char *d = e + 2; size_t nd = strlen(d); if (nd >= 3) return;
    memmove(d + (3 - nd), d, nd + 1); memset(d, '0', 3 - nd);
}
static void fmt_float(Buf *b, char conv, double v, int prec, int width, int left, int zero, int plus, int space, int alt) {
    char body[512]; char sign = signbit(v) ? '-' : plus ? '+' : space ? ' ' : 0;
    if (prec < 0) prec = 6;
    if (prec > 300) prec = 300;
    if (isinf(v) || isnan(v)) {
        /* 1.#INF00, -1.#IND00 (the x87's default NaN), 1.#QNAN0 */
        const char *tag = isinf(v) ? "#INF" : signbit(v) ? "#IND" : "#QNAN";
        char digits[320]; snprintf(digits, sizeof digits, "%s", tag); size_t tl = strlen(digits);
        int p = (conv == 'g' || conv == 'G') ? (prec ? prec - 1 : 0) : prec;
        if ((conv == 'g' || conv == 'G') && !alt) p = (int)tl;           /* %g drops the padding zeros */
        if ((int)tl < p) { memset(digits + tl, '0', (size_t)p - tl); digits[p] = 0; } else if (p >= 0 && (int)tl > p) digits[p] = 0;
        int k = 0; if (sign) body[k++] = sign;
        k += snprintf(body + k, sizeof body - (size_t)k, "1%s%s", p ? "." : "", digits);
        if (conv == 'e' || conv == 'E') snprintf(body + k, sizeof body - (size_t)k, "%c+000", conv);
        if (conv == 'E' || conv == 'G') for (char *q = body; *q; q++) *q = (char)toupper((unsigned char)*q);
        pad(b, body, width, left, 0); return;
    }
    char spec[16]; int k = 0; spec[k++] = '%'; if (alt) spec[k++] = '#'; spec[k++] = '.'; spec[k++] = '*'; spec[k++] = conv == 'F' ? 'f' : conv; spec[k] = 0;
    int off = 0; if (sign) body[off++] = sign;
    snprintf(body + off, sizeof body - (size_t)off, spec, prec, fabs(v));
    fix_exponent(body);
    pad(b, body, width, left, zero);
}
static void fmt_int(Buf *b, char conv, uint64_t u, int is_neg, int prec, int width, int left, int zero, int plus, int space, int alt) {
    char digits[80]; int n = 0; const char *dig = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
    unsigned base = conv == 'o' ? 8 : (conv == 'x' || conv == 'X') ? 16 : 10;
    while (u) { digits[n++] = dig[u % base]; u /= base; }
    if (prec < 0) { if (!n) digits[n++] = '0'; } else { zero = 0; while (n < prec) digits[n++] = '0'; }
    char body[160]; int k = 0;
    if (is_neg) body[k++] = '-'; else if ((conv == 'd' || conv == 'i') && plus) body[k++] = '+'; else if ((conv == 'd' || conv == 'i') && space) body[k++] = ' ';
    if (alt && base == 16 && n && !(n == 1 && digits[0] == '0')) { body[k++] = '0'; body[k++] = conv; }
    if (alt && base == 8 && (n == 0 || digits[n - 1] != '0')) digits[n++] = '0';
    while (n) body[k++] = digits[--n];
    body[k] = 0; pad(b, body, width, left, zero);
}
/* formats f with the arguments at guest address va; returns the length; *out is malloc'ed by the caller's free() */
int crt_format(char **out, const char *f, uint32_t va, int wide_default) {
    Buf b = { 0 }; put(&b, "", 0);
    #define ARG32() (va += 4, RD32(va - 4))
    #define ARG64() (va += 8, RD64(va - 8))
    while (*f) {
        if (*f != '%') { const char *e = f; while (*e && *e != '%') e++; put(&b, f, (size_t)(e - f)); f = e; continue; }
        f++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = 0, prec = -1;
        for (;; f++) { if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1; else if (*f == '#') alt = 1; else if (*f == '0') zero = 1; else break; }
        if (*f == '*') { int w = (int)ARG32(); if (w < 0) { left = 1; w = -w; } width = w; f++; } else while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        if (*f == '.') { f++; prec = 0; if (*f == '*') { int p = (int)ARG32(); prec = p < 0 ? -1 : p; f++; } else while (isdigit((unsigned char)*f)) prec = prec * 10 + (*f++ - '0'); }
        int size = 0;      /* 0 int, 1 short, 2 long, 3 int64, 4 wide (l on c/s), 5 narrow (h on c/s) */
        for (;;) {
            if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { size = 3; f += 3; }
            else if (f[0] == 'I' && f[1] == '3' && f[2] == '2') { size = 0; f += 3; }
            else if (f[0] == 'I') { f++; }                                      /* %Ix: pointer-sized */
            else if (f[0] == 'l' && f[1] == 'l') { size = 3; f += 2; }
            else if (*f == 'l' || *f == 'w') { size = 2; f++; }
            else if (*f == 'h') { size = 1; f++; }
            else if (*f == 'L') { size = 3; f++; }
            else break;
        }
        char conv = *f ? *f++ : 0;
        switch (conv) {
        case 'd': case 'i': {
            int64_t v = size == 3 ? (int64_t)ARG64() : size == 1 ? (int16_t)ARG32() : (int32_t)ARG32();
            fmt_int(&b, conv, v < 0 ? (uint64_t)-(v + 1) + 1 : (uint64_t)v, v < 0, prec, width, left, zero, plus, space, alt); break; }
        case 'u': case 'o': case 'x': case 'X': {
            uint64_t v = size == 3 ? ARG64() : size == 1 ? (uint16_t)ARG32() : (uint32_t)ARG32();
            fmt_int(&b, conv, v, 0, prec, width, left, zero, plus, space, alt); break; }
        case 'p': { char t[16]; snprintf(t, sizeof t, "%08X", (unsigned)ARG32()); pad(&b, t, width, left, 0); break; }
        case 'c': case 'C': {
            uint32_t ch = ARG32(); int wide = size == 2 || (conv == 'C' && size != 1) || (wide_default && conv == 'c' && size != 1);
            char t[2] = { (char)(wide ? (ch < 256 ? ch : '?') : (ch & 0xFF)), 0 };
            if (left) { put(&b, t, 1); putc_n(&b, ' ', width - 1); } else { putc_n(&b, zero ? '0' : ' ', width - 1); put(&b, t, 1); }
            break; }
        case 's': case 'S': {
            uint32_t a = ARG32(); int wide = size == 2 || (conv == 'S' && size != 1) || (wide_default && conv == 's' && size != 1);
            char *tmp; size_t n;
            if (!a) { n = 6; tmp = malloc(7); memcpy(tmp, "(null)", 7); }
            else if (wide) { n = 0; while ((prec < 0 || (int)n < prec) && RD16(a + 2 * (uint32_t)n)) n++; tmp = malloc(n + 1); for (size_t i = 0; i < n; i++) { uint16_t w = RD16(a + 2 * (uint32_t)i); tmp[i] = (char)(w < 256 ? w : '?'); } tmp[n] = 0; }
            else { const char *s = gs(a); n = 0; while ((prec < 0 || (int)n < prec) && s[n]) n++; tmp = malloc(n + 1); memcpy(tmp, s, n); tmp[n] = 0; }
            if (prec >= 0 && n > (size_t)prec) { tmp[prec] = 0; n = (size_t)prec; }
            if ((int)n >= width) put(&b, tmp, n); else if (left) { put(&b, tmp, n); putc_n(&b, ' ', width - (int)n); } else { putc_n(&b, zero ? '0' : ' ', width - (int)n); put(&b, tmp, n); }
            free(tmp); break; }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            double d; uint64_t raw = ARG64(); memcpy(&d, &raw, 8);
            fmt_float(&b, conv, d, prec, width, left, zero, plus, space, alt); break; }
        case 'n': { uint32_t p = ARG32(); if (p) { if (size == 1) WR16(p, (uint16_t)b.n); else WR32(p, (uint32_t)b.n); } break; }
        case '%': put(&b, "%", 1); break;
        case 0: break;
        default: put(&b, &conv, 1); break;        /* MSVC prints an unknown conversion character as itself */
        }
    }
    #undef ARG32
    #undef ARG64
    *out = b.s; return (int)b.n;
}

/* ---------------------------------------------------------------- scanf */
typedef struct { const uint8_t *s; Stream *st; int pos; } Src;
static int sg(Src *r) { int ch = r->st ? stream_getc(r->st) : (r->s[r->pos] ? r->s[r->pos] : -1); if (ch >= 0) r->pos++; return ch; }
static void su(Src *r, int ch) { if (ch < 0) return; r->pos--; if (r->st) stream_ungetc(r->st, ch); }
static int scan(Src *r, const char *f, uint32_t va) {
    int count = 0, any_input = 0;
    #define ARG32() (va += 4, RD32(va - 4))
    while (*f) {
        if (isspace((unsigned char)*f)) { int ch; while ((ch = sg(r)) >= 0 && isspace(ch)) ; su(r, ch); f++; continue; }
        if (*f != '%' || f[1] == '%') {
            int want = *f == '%' ? '%' : (unsigned char)*f; f += *f == '%' ? 2 : 1;
            int ch = sg(r); if (ch < 0) return any_input || count ? count : -1;
            if (ch != want) { su(r, ch); return count; }
            any_input = 1; continue;
        }
        f++;
        int supp = 0, width = 0, size = 0; if (*f == '*') { supp = 1; f++; }
        while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
        for (;;) { if (f[0] == 'I' && f[1] == '6' && f[2] == '4') { size = 3; f += 3; } else if (f[0] == 'l' && f[1] == 'l') { size = 3; f += 2; } else if (*f == 'l' || *f == 'L') { size = size ? size : 2; f++; } else if (*f == 'h') { size = 1; f++; } else break; }
        char cv = *f ? *f++ : 0; if (!cv) break;
        if (cv == 'n') { if (!supp) WR32(ARG32(), (uint32_t)r->pos); continue; }
        if (cv != 'c' && cv != '[') { int ch; while ((ch = sg(r)) >= 0 && isspace(ch)) ; if (ch < 0) return any_input || count ? count : -1; su(r, ch); }
        int ch = -1;
        if (cv == 'c') {
            int w = width ? width : 1; uint32_t d = supp ? 0 : ARG32(); int k = 0;
            while (k < w && (ch = sg(r)) >= 0) { if (d) WR8(d + (uint32_t)k, (uint8_t)ch); k++; }
            if (!k) return count ? count : -1; if (!supp) count++; any_input = 1; continue;
        }
        if (cv == 's' || cv == '[') {
            char set[256]; int neg = 0;
            if (cv == '[') {
                memset(set, 0, sizeof set); if (*f == '^') { neg = 1; f++; } if (*f == ']') { set[']'] = 1; f++; }
                while (*f && *f != ']') { if (f[1] == '-' && f[2] && f[2] != ']') { for (int q = (unsigned char)f[0]; q <= (unsigned char)f[2]; q++) set[q] = 1; f += 3; } else set[(unsigned char)*f++] = 1; }
                if (*f == ']') f++;
            }
            uint32_t d = supp ? 0 : ARG32(); int n = 0, w = width ? width : 0x7FFFFFFF;
            while (n < w && (ch = sg(r)) >= 0) {
                int stop = cv == 's' ? isspace(ch) : (set[ch] == neg);
                if (stop) { su(r, ch); break; }
                if (d) WR8(d + (uint32_t)n, (uint8_t)ch); n++;
            }
            if (!n) return count || any_input ? count : -1;
            if (d) { WR8(d + (uint32_t)n, 0); count++; } any_input = 1; continue;
        }
        /* numbers: collect the longest valid prefix, then convert */
        char buf[512]; int n = 0, w = width ? width : 511; if (w > 511) w = 511;
        int isfloat = cv == 'f' || cv == 'e' || cv == 'g' || cv == 'E' || cv == 'G';
        int base = cv == 'x' || cv == 'X' ? 16 : cv == 'o' ? 8 : cv == 'i' ? 0 : 10;
        int seen_digit = 0, seen_dot = 0, seen_e = 0;
        while (n < w && (ch = sg(r)) >= 0) {
            int ok = 0;
            if ((ch == '-' || ch == '+') && (n == 0 || (isfloat && (buf[n - 1] == 'e' || buf[n - 1] == 'E')))) ok = 1;
            else if (isdigit(ch) && (base != 8 || ch < '8')) { ok = 1; seen_digit = 1; }
            else if (isfloat && ch == '.' && !seen_dot && !seen_e) { ok = 1; seen_dot = 1; }
            else if (isfloat && (ch == 'e' || ch == 'E') && seen_digit && !seen_e) { ok = 1; seen_e = 1; }
            else if ((base == 16 || base == 0) && (ch == 'x' || ch == 'X') && n >= 1 && buf[n - 1] == '0' && (n == 1 || (n == 2 && (buf[0] == '-' || buf[0] == '+')))) { ok = 1; if (!base) base = 16; }
            else if ((base == 16) && isxdigit(ch)) { ok = 1; seen_digit = 1; }
            else if (base == 0 && isxdigit(ch) && n >= 2 && (buf[n - 1] == 'x' || buf[n - 1] == 'X' || isxdigit((unsigned char)buf[n - 1]))) { ok = 1; seen_digit = 1; }
            if (!ok) { su(r, ch); break; }
            buf[n++] = (char)ch;
        }
        buf[n] = 0;
        if (!seen_digit) return count || any_input ? count : (ch < 0 ? -1 : 0);
        any_input = 1;
        if (supp) continue;
        uint32_t d = ARG32();
        if (isfloat) { double v = strtod(buf, NULL); if (size >= 2) { uint64_t raw; memcpy(&raw, &v, 8); WR64(d, raw); } else { float fv = (float)v; uint32_t raw; memcpy(&raw, &fv, 4); WR32(d, raw); } }
        else {
            int bb = cv == 'd' || cv == 'u' ? 10 : cv == 'i' ? 0 : base;
            uint64_t v = (buf[0] == '-') ? (uint64_t)strtoll(buf, NULL, bb) : strtoull(buf, NULL, bb);
            if (size == 3) WR64(d, v); else if (size == 1) WR16(d, (uint16_t)v); else WR32(d, (uint32_t)v);
        }
        count++;
    }
    #undef ARG32
    return count;
}
int crt_sscanf(const char *s, const char *f, uint32_t va) { Src r = { (const uint8_t *)s, NULL, 0 }; return scan(&r, f, va); }
int crt_fscanf(Stream *st, const char *f, uint32_t va) { Src r = { NULL, st, 0 }; return scan(&r, f, va); }
