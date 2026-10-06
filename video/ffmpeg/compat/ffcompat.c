/* Runtime pieces for ffcompat.h (memory, frames, tables). Portable C99. */
#include "ffcompat.h"

/* 16-byte aligned allocations without platform calls: over-allocate and keep the original pointer just before the block */
void *av_malloc(size_t size) {
    if (size > INT_MAX) return NULL;
    uint8_t *raw = (uint8_t *)malloc(size + 32); if (!raw) return NULL;
    uint8_t *p = (uint8_t *)(((uintptr_t)raw + 16 + 15) & ~(uintptr_t)15);
    ((void **)p)[-1] = raw; return p;
}
void av_free(void *p) { if (p) free(((void **)p)[-1]); }
void av_freep(void *pp) { void **q = (void **)pp; av_free(*q); *q = NULL; }
void *av_mallocz(size_t size) { void *p = av_malloc(size); if (p) memset(p, 0, size); return p; }
void *av_calloc(size_t n, size_t size) { return size && n > INT_MAX / size ? NULL : av_mallocz(n * size); }
void *av_malloc_array(size_t n, size_t size) { return size && n > INT_MAX / size ? NULL : av_malloc(n * size); }
void *av_realloc_f(void *p, size_t n, size_t size) {   /* only used to grow small tables: allocate, copy what we can, free */
    if (size && n > INT_MAX / size) { av_free(p); return NULL; }
    void *q = av_malloc(n * size); if (!q) { av_free(p); return NULL; }
    if (p) { memcpy(q, p, n * size); av_free(p); }   /* old block is at least as large in every use here */
    return q;
}

const uint8_t ff_reverse[256] = {
#define R2(n) n, n + 2*64, n + 1*64, n + 3*64
#define R4(n) R2(n), R2(n + 2*16), R2(n + 1*16), R2(n + 3*16)
#define R6(n) R4(n), R4(n + 2*4), R4(n + 1*4), R4(n + 3*4)
    R6(0), R6(2), R6(1), R6(3)
};

int ff_set_dimensions(AVCodecContext *avctx, int w, int h) {
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) return AVERROR_INVALIDDATA;
    avctx->width = avctx->coded_width = w; avctx->height = avctx->coded_height = h; return 0;
}
int av_image_check_size2(unsigned w, unsigned h, int64_t max_pixels, enum AVPixelFormat fmt, int log_offset, void *log_ctx) {
    (void)fmt; (void)log_offset; (void)log_ctx;
    if (!w || !h || w > 4096 || h > 4096 || (max_pixels > 0 && (int64_t)w * h > max_pixels)) return AVERROR(EINVAL);
    return 0;
}
/* YUV 4:1:0 planar, each plane padded and 32-byte aligned */
int ff_get_buffer(AVCodecContext *avctx, AVFrame *f, int flags) {
    (void)flags;
    int w = FFALIGN(avctx->width, 32) + 32, h = FFALIGN(avctx->height, 16) + 16;
    int cw = w / 4 + 32, ch = h / 4 + 4;
    av_frame_unref(f);
    f->buf = (uint8_t *)av_mallocz((size_t)w * h + 2 * (size_t)cw * ch + 64);
    if (!f->buf) return AVERROR(ENOMEM);
    f->data[0] = f->buf; f->linesize[0] = w;
    f->data[1] = f->buf + (size_t)w * h; f->linesize[1] = cw;
    f->data[2] = f->data[1] + (size_t)cw * ch; f->linesize[2] = cw;
    f->width = avctx->width; f->height = avctx->height; f->format = AV_PIX_FMT_YUV410P;
    return 0;
}
void av_frame_unref(AVFrame *f) { av_free(f->buf); memset(f, 0, sizeof *f); }
void av_frame_move_ref(AVFrame *dst, AVFrame *src) { *dst = *src; memset(src, 0, sizeof *src); }
void av_frame_free(AVFrame **f) { if (*f) { av_frame_unref(*f); av_free(*f); *f = NULL; } }

const uint8_t ff_zigzag_direct[64] = {
    0,   1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};
