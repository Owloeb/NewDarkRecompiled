/* lavshim_test <file.avi> <outprefix>: drive lavshim the way lgvid.dll does (same calls, same order, same arguments) and write a few
   scaled frames (PPM) plus the decoded audio (WAV). Build as a 32-bit program with RT_IDENTITY (guest address == host address). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include "lavshim.h"

uint8_t *M;
#define READFN 0x1000u
static FILE *fin;

uint32_t lavh_alloc(uint32_t n) { void *p = aligned_alloc(16, (n + 15) & ~15u); if (p) memset(p, 0, n); return (uint32_t)(uintptr_t)p; }
void lavh_free(uint32_t p) { free((void *)(uintptr_t)p); }
void lavh_call(CPU *c, uint32_t fn, int n, const uint32_t *a) {
    if (fn != READFN || n != 3) { fprintf(stderr, "unexpected guest call %08x\n", fn); exit(1); }
    c->eax = (uint32_t)fread((void *)(uintptr_t)a[1], 1, a[2], fin);
}
uint32_t lavh_fnaddr(lav_fn f) { (void)f; return 0x2000u; }
int64_t lavh_time_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
void lavh_lock(void) {}
void lavh_unlock(void) {}
void lavh_log(const char *f, ...) { va_list a; va_start(a, f); vfprintf(stderr, f, a); fputc('\n', stderr); va_end(a); }

static CPU cpu; static uint32_t gstack[16384];
static lav_fn fnd(const char *n) { for (const LavExport *e = lav_exports; e->name; e++) if (!strcmp(e->name, n)) return e->fn; fprintf(stderr, "no export %s\n", n); exit(1); }
static uint32_t call(const char *name, int n, ...) {
    uint32_t sp = (uint32_t)(uintptr_t)&gstack[16000]; va_list a; va_start(a, n);
    sp -= 4 * n; for (int i = 0; i < n; i++) WR32(sp + 4 * i, va_arg(a, uint32_t)); va_end(a);
    sp -= 4; WR32(sp, 0xdeadbeef); cpu.esp = sp; fnd(name)(&cpu); return cpu.eax;
}
#define G(p) ((uint32_t)(uintptr_t)(p))

int main(int argc, char **argv) {
    if (argc < 3 || !(fin = fopen(argv[1], "rb"))) { fprintf(stderr, "usage: lavshim_test file.avi outprefix\n"); return 1; }
    const int DW = 800, DH = 600, DF = 30 /* BGRA */;
    call("avcodec_init", 0); call("av_register_all", 0);
    uint32_t ic = call("avformat_alloc_context", 0);
    uint32_t pb = call("avio_alloc_context", 7, 0, 0, 0, 0x5555u, READFN, 0, 0x3000u);
    uint32_t fmt = 0; if ((int)call("av_probe_input_buffer", 6, pb, G(&fmt), G(argv[1]), 0, 0, 0) < 0) { fprintf(stderr, "probe failed\n"); return 1; }
    uint32_t icp = ic;
    if ((int)call("av_open_input_stream", 5, G(&icp), pb, G(argv[1]), fmt, 0) < 0) { fprintf(stderr, "open failed\n"); return 1; }
    ic = icp;
    call("av_find_stream_info", 1, ic);
    int vi = (int)call("av_find_best_stream", 6, ic, 0, -1, -1, 0, 0), ai = (int)call("av_find_best_stream", 6, ic, 1, -1, vi, 0, 0);
    printf("video stream %d, audio stream %d\n", vi, ai);
    uint32_t vst = RD32(ic + 0x18 + 4 * vi), vcc = RD32(vst + 8), acc = ai >= 0 ? RD32(RD32(ic + 0x18 + 4 * ai) + 8) : 0;
    if ((int)call("avcodec_open", 2, vcc, call("avcodec_find_decoder", 1, RD32(vcc + 0xe0))) < 0) { fprintf(stderr, "video open failed\n"); return 1; }
    if (acc && (int)call("avcodec_open", 2, acc, call("avcodec_find_decoder", 1, RD32(acc + 0xe0))) < 0) { fprintf(stderr, "audio open failed\n"); return 1; }
    printf("video %ux%u pixfmt %d tb %u/%u; audio %u Hz %u ch fmt %d\n", RD32(vcc + 0x28), RD32(vcc + 0x2c), (int)RD32(vcc + 0x34), RD32(vst + 0x38), RD32(vst + 0x3c),
           acc ? RD32(acc + 0x40) : 0, acc ? RD32(acc + 0x44) : 0, acc ? (int)RD32(acc + 0x48) : -1);
    uint32_t frame = call("avcodec_alloc_frame", 0), sws = 0;
    uint8_t *rgb = malloc((size_t)DW * DH * 4), *pcm = malloc(64 << 20); uint32_t pcmlen = 0; int16_t *abuf = malloc(0x46500);
    int nv = 0, ngot = 0, na = 0; int64_t lastpts = -1, t0 = lavh_time_us(), tscale = 0;
    uint8_t pkt[0x40], pk2[0x40];
    for (;;) {
        call("av_init_packet", 1, G(pkt));
        if ((int)call("av_read_frame", 2, ic, G(pkt)) < 0) break;
        call("av_dup_packet", 1, G(pkt));
        int si = (int)RD32(G(pkt) + 0x18);
        if (si == vi) {
            nv++; uint32_t got = 0; call("avcodec_decode_video2", 4, vcc, frame, G(&got), G(pkt));
            if (got) {
                ngot++; int64_t pts = (int64_t)RD64(frame + 0xf0); if (pts <= lastpts) printf("pts not increasing: %lld after %lld\n", (long long)pts, (long long)lastpts); lastpts = pts;
                int64_t t1 = lavh_time_us();
                sws = call("sws_getCachedContext", 11, sws, RD32(vcc + 0x28), RD32(vcc + 0x2c), RD32(vcc + 0x34), DW, DH, DF, 4, 0, 0, 0);
                uint32_t dst[4] = { G(rgb), 0, 0, 0 }; int32_t ds[4] = { DW * 4, 0, 0, 0 };
                if ((int)call("sws_scale", 7, sws, frame, frame + 0x10, 0, RD32(vcc + 0x2c), G(dst), G(ds)) != DH) { fprintf(stderr, "sws_scale failed\n"); return 1; }
                tscale += lavh_time_us() - t1;
                if (pts == 40 || pts == 120 || pts == 200) {
                    char nm[512]; snprintf(nm, sizeof nm, "%s_%03lld.ppm", argv[2], (long long)pts); FILE *f = fopen(nm, "wb"); fprintf(f, "P6\n%d %d\n255\n", DW, DH);
                    for (int i = 0; i < DW * DH; i++) { uint8_t px[3] = { rgb[4 * i + 2], rgb[4 * i + 1], rgb[4 * i] }; fwrite(px, 1, 3, f); } fclose(f);
                }
            }
        } else if (si == ai) {   /* like lgvid's mixer callback: decode until the packet is used up */
            na++; memcpy(pk2, pkt, 0x40);
            while ((int)RD32(G(pk2) + 0x14) > 0) {
                int32_t fs = 0x46500; int r = (int)call("avcodec_decode_audio3", 4, acc, G(abuf), G(&fs), G(pk2));
                if (r < 0) { fprintf(stderr, "audio decode error\n"); break; }
                WR32(G(pk2) + 0x10, RD32(G(pk2) + 0x10) + r); WR32(G(pk2) + 0x14, RD32(G(pk2) + 0x14) - r);
                if (fs > 0 && pcmlen + fs < (64u << 20)) { memcpy(pcm + pcmlen, abuf, fs); pcmlen += fs; }
            }
        }
        call("av_free_packet", 1, G(pkt));
    }
    printf("%d video packets, %d pictures (last pts %lld), %d audio packets, %u bytes PCM (%.2f s), total %.2f s, scaling %.2f ms/frame\n",
           nv, ngot, (long long)lastpts, na, pcmlen, acc ? pcmlen / (2.0 * RD32(acc + 0x44) * RD32(acc + 0x40)) : 0.0, (lavh_time_us() - t0) / 1e6, ngot ? tscale / 1000.0 / ngot : 0.0);
    if (acc) {
        char nm[512]; snprintf(nm, sizeof nm, "%s.wav", argv[2]); FILE *f = fopen(nm, "wb"); uint32_t sr = RD32(acc + 0x40), ch = RD32(acc + 0x44);
        uint8_t h[44]; memcpy(h, "RIFF", 4); WR32(G(h + 4), 36 + pcmlen); memcpy(h + 8, "WAVEfmt ", 8); WR32(G(h + 16), 16); WR16(G(h + 20), 1); WR16(G(h + 22), ch);
        WR32(G(h + 24), sr); WR32(G(h + 28), sr * ch * 2); WR16(G(h + 32), ch * 2); WR16(G(h + 34), 16); memcpy(h + 36, "data", 4); WR32(G(h + 40), pcmlen);
        fwrite(h, 1, 44, f); fwrite(pcm, 1, pcmlen, f); fclose(f);
    }
    call("av_free", 1, frame); call("sws_freeContext", 1, sws);
    call("avcodec_close", 1, vcc); if (acc) call("avcodec_close", 1, acc);
    call("av_close_input_stream", 1, ic); call("av_free", 1, pb);
    return 0;
}
