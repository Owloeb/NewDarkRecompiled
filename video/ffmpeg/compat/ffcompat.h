/* Minimal stand-ins for the parts of FFmpeg's libavutil/libavcodec that the extracted Indeo 5 decoder uses.
   Plain portable C99; no FFmpeg build system, no threads, no platform code. */
#ifndef FFCOMPAT_H
#define FFCOMPAT_H
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#define ARCH_X86 0
#define ARCH_ARM 0
#define ARCH_AARCH64 0
#define ARCH_PPC 0
#define ARCH_MIPS 0
#define ARCH_LOONGARCH 0
#define ARCH_RISCV 0
#define HAVE_FAST_CLZ 0
#define HAVE_FAST_64BIT 0
#define EXTERN extern
#define HAVE_INLINE_ASM 0
#define HAVE_X86ASM 0
#define HAVE_THREADS 0
#define HAVE_BIGENDIAN 0
#define CONFIG_INDEO4_DECODER 0
#define CONFIG_INDEO5_DECODER 1
#define CACHED_BITSTREAM_READER 0
#define UNCHECKED_BITSTREAM_READER 0

#define av_cold
#define av_unused __attribute__((unused))
#define av_const
#define av_pure
#define av_always_inline inline __attribute__((always_inline))
#define av_noinline __attribute__((noinline))
#define av_flatten
#define av_builtin_constant_p __builtin_constant_p
#define av_unreachable(msg) __builtin_unreachable()
#define av_fallthrough __attribute__((fallthrough))
#define attribute_visibility_hidden
#define av_warn_unused_result
#define av_assert0(c) do { if (!(c)) abort(); } while (0)
#define av_assert1(c) ((void)0)
#define av_assert2(c) ((void)0)
#define av_log(ctx, lvl, ...) ((void)0)
#define ff_dlog(ctx, ...) ((void)0)
#define ff_tlog(ctx, ...) ((void)0)
#define avpriv_request_sample(ctx, ...) ((void)0)
#define avpriv_report_missing_feature(ctx, ...) ((void)0)
#define AV_LOG_ERROR 16
#define AV_LOG_WARNING 24
#define AV_LOG_DEBUG 48

#define FFMIN(a,b) ((a) > (b) ? (b) : (a))
#define FFMAX(a,b) ((a) > (b) ? (a) : (b))
#define FFABS(a) ((a) >= 0 ? (a) : (-(a)))
#define FFSIGN(a) ((a) > 0 ? 1 : -1)
#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define FFSWAP(type,a,b) do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)
#define FF_ARRAY_ELEMS(a) (sizeof(a) / sizeof((a)[0]))
#define FF_SIGNBIT(x) ((x) >> CHAR_BIT * sizeof(x) - 1)
#define MKTAG(a,b,c,d) ((a) | ((b) << 8) | ((c) << 16) | ((unsigned)(d) << 24))

static inline int av_clip(int a, int amin, int amax) { return a < amin ? amin : a > amax ? amax : a; }
static inline uint8_t av_clip_uint8(int a) { return a & ~0xFF ? (~a) >> 31 : a; }
static inline int16_t av_clip_int16(int a) { return (a + 0x8000U) & ~0xFFFF ? (a >> 31) ^ 0x7FFF : a; }
static inline unsigned av_clip_uintp2(int a, int p) { return a & ~((1u << p) - 1) ? (~a) >> 31 & ((1u << p) - 1) : a; }
static inline int av_log2(unsigned v) { return v ? 31 - __builtin_clz(v) : 0; }
static inline int av_log2_16bit(unsigned v) { return av_log2(v & 0xFFFF); }

/* errors */
#define AVERROR(e) (-(e))
#define FFERRTAG(a, b, c, d) (-(int)MKTAG(a, b, c, d))
#define AVERROR_INVALIDDATA FFERRTAG('I','N','D','A')
#define AVERROR_PATCHWELCOME FFERRTAG('P','A','W','E')
#define AVERROR_BUG FFERRTAG('B','U','G','!')

/* memory */
void *av_malloc(size_t size);
void *av_mallocz(size_t size);
void *av_calloc(size_t n, size_t size);
void *av_malloc_array(size_t n, size_t size);
void *av_realloc_f(void *p, size_t n, size_t size);
void av_free(void *p);
void av_freep(void *pp);

/* byte access */
#define AV_RL16(p) ((uint16_t)(((const uint8_t *)(p))[0] | ((const uint8_t *)(p))[1] << 8))
#define AV_RL32(p) ((uint32_t)(((const uint8_t *)(p))[0] | ((const uint8_t *)(p))[1] << 8 | ((const uint8_t *)(p))[2] << 16 | (uint32_t)((const uint8_t *)(p))[3] << 24))
#define AV_RB16(p) ((uint16_t)(((const uint8_t *)(p))[0] << 8 | ((const uint8_t *)(p))[1]))
#define AV_RB32(p) ((uint32_t)((uint32_t)((const uint8_t *)(p))[0] << 24 | ((const uint8_t *)(p))[1] << 16 | ((const uint8_t *)(p))[2] << 8 | ((const uint8_t *)(p))[3]))
#define AV_RB64(p) ((uint64_t)AV_RB32(p) << 32 | AV_RB32((const uint8_t *)(p) + 4))
#define AV_RL64(p) ((uint64_t)AV_RL32((const uint8_t *)(p) + 4) << 32 | AV_RL32(p))
#define AV_COPY64(d, s) memcpy((d), (s), 8)
#define AV_INPUT_BUFFER_PADDING_SIZE 64

/* once (no threads in this build: a plain flag) */
typedef int AVOnce;
#define AV_ONCE_INIT 0
static inline int ff_thread_once(AVOnce *o, void (*fn)(void)) { if (!*o) { *o = 1; fn(); } return 0; }

/* qsort / bit reversal */
#define AV_QSORT(p, num, type, cmp) qsort((p), (num), sizeof(type), (int (*)(const void *, const void *))(cmp))
extern const uint8_t ff_reverse[256];

/* the slice of libavcodec the decoder touches */
enum AVPixelFormat { AV_PIX_FMT_NONE = -1, AV_PIX_FMT_YUV410P = 1 };
enum AVMediaType { AVMEDIA_TYPE_VIDEO = 0 };
enum AVCodecID { AV_CODEC_ID_NONE, AV_CODEC_ID_INDEO4, AV_CODEC_ID_INDEO5 };
#define AV_CODEC_CAP_DR1 2
#define FF_CODEC_CAP_INIT_CLEANUP 2
#define AV_NUM_DATA_POINTERS 8
typedef struct AVFrame { uint8_t *data[AV_NUM_DATA_POINTERS]; int linesize[AV_NUM_DATA_POINTERS]; int width, height, format; uint8_t *buf; } AVFrame;
typedef struct AVPacket { const uint8_t *data; int size; } AVPacket;
typedef struct AVCodecContext {
    void *priv_data; int width, height, coded_width, coded_height; enum AVPixelFormat pix_fmt; enum AVCodecID codec_id;
    int64_t max_pixels; int has_b_frames;
} AVCodecContext;
typedef struct FFCodecPublic { const char *name; enum AVMediaType type; enum AVCodecID id; int capabilities; } FFCodecPublic;
typedef struct FFCodec {
    FFCodecPublic p; int priv_data_size, caps_internal, long_name_unused;
    int (*init)(AVCodecContext *); int (*close)(AVCodecContext *);
    struct { int (*decode)(AVCodecContext *, AVFrame *, int *, AVPacket *); } cb;
} FFCodec;
#define CODEC_LONG_NAME(s) .long_name_unused = 0
#define FF_CODEC_DECODE_CB(f) .cb.decode = (f)
int ff_get_buffer(AVCodecContext *avctx, AVFrame *frame, int flags);
int ff_set_dimensions(AVCodecContext *avctx, int w, int h);
int av_image_check_size2(unsigned w, unsigned h, int64_t max_pixels, enum AVPixelFormat fmt, int log_offset, void *log_ctx);
void av_frame_unref(AVFrame *f);
void av_frame_move_ref(AVFrame *dst, AVFrame *src);
void av_frame_free(AVFrame **f);
#endif
