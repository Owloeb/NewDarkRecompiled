/* lavshim: portable replacement for the 2011 ffmpeg.dll used by lgvid.dll (see lavshim.h).
 *
 * What lgvid.dll does with ffmpeg.dll (from its disassembly; 29 of the 39 functions it resolves are ever called):
 *   avformat_alloc_context, avio_alloc_context(read/seek callbacks into lgvid), av_probe_input_buffer,
 *   av_open_input_stream, av_find_stream_info, av_find_best_stream (video, then audio), avcodec_find_decoder/open/close,
 *   a demux thread calling av_read_frame + av_dup_packet, a video thread calling avcodec_decode_video2 and then
 *   sws_getCachedContext + sws_scale into the engine's locked movie surface, and the sound mixer calling
 *   avcodec_decode_audio3 (with av_audio_convert when the decoder's samples are not 16-bit). It reads these struct fields:
 *     AVFormatContext: pb 0x10, nb_streams 0x14, streams[] 0x18          AVStream: codec 0x08, time_base 0x38, discard 0x48
 *     AVCodecContext: time_base 0x20, width 0x28, height 0x2c, pix_fmt 0x34, sample_rate 0x40, channels 0x44,
 *                     sample_fmt 0x48, codec_type 0xdc, codec_id 0xe0, request_channels 0x338
 *     AVFrame: data 0x00, linesize 0x10, repeat_pict 0x9c, best_effort_timestamp 0xf0     AVPacket: 0x40 bytes
 * Offsets are FFmpeg 0.7's (ABI-compatible with the Lavc 52.114 / Lavf 52.103 build the game shipped), 32-bit.
 *
 * Supported content: AVI (incl. OpenDML), Indeo 5 video, PCM 8/16-bit audio - what System Shock 2's cutscenes use.
 * A stream we can't decode is reported as absent, so lgvid plays the rest (or skips the movie) instead of crashing. */
#include <stdlib.h>
#include <string.h>
#include "lavshim.h"
#include "ffcompat.h"

extern const FFCodec ff_indeo5_decoder;

#define ARG(i)   RD32(c->esp + 4 + 4 * (i))
#define NOPTS    0x8000000000000000ull
#define EOF_ERR  ((uint32_t)-32)                 /* AVERROR_EOF in this FFmpeg version (AVERROR(EPIPE)) */
#define INVALID  ((uint32_t)-1094995529)         /* AVERROR_INVALIDDATA */
#define NOTFOUND ((uint32_t)-1381258232)         /* AVERROR_STREAM_NOT_FOUND */
#define ENOMEM_  ((uint32_t)-12)

/* struct sizes and field offsets (32-bit FFmpeg 0.7 ABI) */
enum { SZ_FMTCTX = 0xf90, SZ_STREAM = 0x1e8, SZ_CODECCTX = 0x408, SZ_FRAME = 0x118, SZ_PACKET = 0x40, SZ_IOCTX = 0x58, SZ_CODEC = 0x58 };
enum { FC_IFORMAT = 0x04, FC_PRIV = 0x0c, FC_PB = 0x10, FC_NB_STREAMS = 0x14, FC_STREAMS = 0x18, FC_START = 0xea0, FC_DURATION = 0xea8, FC_MAXSTREAMS = 20 };
enum { ST_INDEX = 0x00, ST_ID = 0x04, ST_CODEC = 0x08, ST_RFRAMERATE = 0x0c, ST_TIMEBASE = 0x38, ST_DISCARD = 0x48, ST_START = 0x50,
       ST_DURATION = 0x58, ST_CURDTS = 0x70, ST_NBFRAMES = 0x98, ST_SAR = 0x168, ST_AVGFRAMERATE = 0x1d0 };
enum { CC_BITRATE = 0x04, CC_EXTRADATA = 0x18, CC_EXTRASIZE = 0x1c, CC_TIMEBASE = 0x20, CC_WIDTH = 0x28, CC_HEIGHT = 0x2c, CC_PIXFMT = 0x34,
       CC_SRATE = 0x40, CC_CHANNELS = 0x44, CC_SAMPLEFMT = 0x48, CC_CODEC = 0x84, CC_PRIV = 0x88, CC_BLOCKALIGN = 0x10c,
       CC_BPCS = 0x180, CC_SAR = 0x188, CC_CODECTYPE = 0xdc, CC_CODECID = 0xe0, CC_CODECTAG = 0xe4, CC_CODEDW = 0x294, CC_CODEDH = 0x298 };
enum { FR_DATA = 0x00, FR_LINESIZE = 0x10, FR_KEY = 0x30, FR_PICTTYPE = 0x34, FR_PTS = 0x38, FR_REPEAT = 0x9c, FR_PKTPTS = 0xd8,
       FR_PKTDTS = 0xe0, FR_BEST = 0xf0, FR_PKTPOS = 0xf8 };
enum { PK_PTS = 0x00, PK_DTS = 0x08, PK_DATA = 0x10, PK_SIZE = 0x14, PK_STREAM = 0x18, PK_FLAGS = 0x1c, PK_DURATION = 0x20,
       PK_DESTRUCT = 0x24, PK_PRIV = 0x28, PK_POS = 0x30, PK_CONVDUR = 0x38 };
enum { IO_BUFFER = 0x00, IO_BUFSIZE = 0x04, IO_OPAQUE = 0x10, IO_READ = 0x14, IO_WRITE = 0x18, IO_SEEK = 0x1c, IO_WRITEFLAG = 0x30 };
enum { CO_NAME = 0x00, CO_TYPE = 0x04, CO_ID = 0x08 };
/* enum values */
enum { T_VIDEO = 0, T_AUDIO = 1 };
enum { ID_NONE = 0, ID_INDEO5 = 116, ID_PCM_S16LE = 65536, ID_PCM_U8 = 65541 };
enum { PF_YUV420P = 0, PF_RGB24 = 2, PF_BGR24 = 3, PF_YUV410P = 6, PF_ARGB = 27, PF_RGBA = 28, PF_ABGR = 29, PF_BGRA = 30,
       PF_RGB565LE = 44, PF_BGR565LE = 48 };
enum { SF_U8 = 0, SF_S16 = 1, SF_S32 = 2, SF_FLT = 3, SF_DBL = 4 };
enum { SWS_FAST_BILINEAR = 1, SWS_BILINEAR = 2, SWS_BICUBIC = 4, SWS_POINT = 0x10, SWS_AREA = 0x20 };

static int g_logged;
#define LOG(...) do { if (g_logged++ < 200) lavh_log("LAVSHIM " __VA_ARGS__); } while (0)

/* ---------------------------------------------------------------- host-side objects, referenced from guest structs by small ids */
enum { K_IO = 1, K_DEMUX, K_DEC, K_SWS, K_CONV };
typedef struct { int kind; } Obj;
#define MAXOBJ 128
static Obj *objs[MAXOBJ];
static uint32_t obj_put(Obj *o) {
    lavh_lock();
    for (uint32_t i = 1; i < MAXOBJ; i++) if (!objs[i]) { objs[i] = o; lavh_unlock(); return i; }
    lavh_unlock(); return 0;
}
static Obj *obj_get(uint32_t id, int kind) {
    lavh_lock(); Obj *o = id && id < MAXOBJ ? objs[id] : NULL; lavh_unlock();
    return o && o->kind == kind ? o : NULL;
}
static void obj_del(uint32_t id) { lavh_lock(); if (id && id < MAXOBJ) objs[id] = NULL; lavh_unlock(); }

static uint32_t gstr(const char *s) {   /* a constant string in guest memory (kept for the process lifetime) */
    uint32_t n = (uint32_t)strlen(s) + 1, g = lavh_alloc(n); if (g) memcpy(GP(g), s, n); return g;
}

/* ---------------------------------------------------------------- byte input through lgvid's own read callback */
#define IOBUF 65536
typedef struct { Obj o; uint32_t pb, opaque, readfn, gbuf; uint32_t pos, len; int eof; uint64_t off; } Io;
static Io *io_of(uint32_t pb) {   /* the Io behind an AVIOContext we allocated */
    lavh_lock();
    for (int i = 1; i < MAXOBJ; i++) if (objs[i] && objs[i]->kind == K_IO && ((Io *)objs[i])->pb == pb) { Io *r = (Io *)objs[i]; lavh_unlock(); return r; }
    lavh_unlock(); return NULL;
}
static int io_fill(CPU *c, Io *io) {   /* refill the guest staging buffer; keeps any unread tail */
    if (io->eof) return 0;
    if (io->pos) { memmove(GP(io->gbuf), GP(io->gbuf + io->pos), io->len - io->pos); io->len -= io->pos; io->pos = 0; }
    if (io->len >= IOBUF) return 1;
    uint32_t a[3] = { io->opaque, io->gbuf + io->len, IOBUF - io->len };
    lavh_call(c, io->readfn, 3, a);
    int n = (int)c->eax;
    if (n <= 0) { io->eof = 1; return 0; }
    io->len += (uint32_t)n; return 1;
}
static int io_need(CPU *c, Io *io, uint32_t n) { while (io->len - io->pos < n) if (!io_fill(c, io)) return 0; return 1; }
static int io_read(CPU *c, Io *io, void *dst, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n) {
        if (io->pos == io->len && !io_fill(c, io)) return 0;
        uint32_t k = io->len - io->pos; if (k > n) k = n;
        if (d) { memcpy(d, GP(io->gbuf + io->pos), k); d += k; }
        io->pos += k; io->off += k; n -= k;
    }
    return 1;
}
static uint32_t rl32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t rl16(const uint8_t *p) { return p[0] | p[1] << 8; }
#define TAG(a, b, c_, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c_) << 16 | (uint32_t)(d) << 24)

/* ---------------------------------------------------------------- AVI demuxer */
typedef struct {
    int type, codec_id; uint32_t handler, scale, rate, length, sample_size;
    int w, h, channels, srate, block_align, bits; uint32_t tag; uint8_t extra[256]; int extra_len;
    int64_t count; uint32_t gstream;
} AviStream;
typedef struct { Obj o; Io *io; uint32_t ic; int nst; AviStream st[FC_MAXSTREAMS]; int in_movi; int nkey; } Demux;

static int codec_for(AviStream *s) {
    if (s->type == T_VIDEO) {
        uint32_t t = s->handler | 0x20202020u, cmp = s->tag | 0x20202020u;   /* case-insensitive FourCC */
        if (cmp == TAG('i','v','5','0') || t == TAG('i','v','5','0')) return ID_INDEO5;
        return ID_NONE;
    }
    if (s->tag == 1 && s->bits == 16) return ID_PCM_S16LE;
    if (s->tag == 1 && s->bits == 8) return ID_PCM_U8;
    return ID_NONE;
}
static int avi_header(CPU *c, Demux *d) {
    Io *io = d->io; uint8_t h[12];
    if (!io_read(c, io, h, 12) || rl32(h) != TAG('R','I','F','F') || rl32(h + 8) != TAG('A','V','I',' ')) return -1;
    AviStream *cur = NULL;
    for (;;) {
        uint8_t ch[8]; if (!io_read(c, io, ch, 8)) return -1;
        uint32_t id = rl32(ch), sz = rl32(ch + 4);
        if (id == TAG('L','I','S','T')) {
            uint8_t t[4]; if (!io_read(c, io, t, 4)) return -1;
            if (rl32(t) == TAG('m','o','v','i')) { d->in_movi = 1; return 0; }
            continue;   /* hdrl, strl, odml...: descend */
        }
        uint32_t pad = sz + (sz & 1);
        if ((id == TAG('s','t','r','h') || id == TAG('s','t','r','f')) && pad <= 4096) {
            uint8_t b[4096]; if (!io_read(c, io, b, pad)) return -1;
            if (id == TAG('s','t','r','h') && sz >= 48) {
                if (d->nst >= FC_MAXSTREAMS) { cur = NULL; continue; }
                cur = &d->st[d->nst++]; memset(cur, 0, sizeof *cur);
                uint32_t type = rl32(b);
                cur->type = type == TAG('v','i','d','s') ? T_VIDEO : type == TAG('a','u','d','s') ? T_AUDIO : -1;
                cur->handler = rl32(b + 4); cur->scale = rl32(b + 20); cur->rate = rl32(b + 24); cur->length = rl32(b + 32); cur->sample_size = rl32(b + 44);
                if (!cur->scale || !cur->rate) { cur->scale = 1; cur->rate = 15; }
            } else if (id == TAG('s','t','r','f') && cur) {
                if (cur->type == T_VIDEO && sz >= 40) { cur->w = (int)rl32(b + 4); cur->h = (int)rl32(b + 8); if (cur->h < 0) cur->h = -cur->h; cur->tag = rl32(b + 16);
                    cur->extra_len = sz > 40 ? (int)(sz - 40 < 256 ? sz - 40 : 256) : 0; memcpy(cur->extra, b + 40, cur->extra_len); }
                if (cur->type == T_AUDIO && sz >= 14) { cur->tag = rl16(b); cur->channels = (int)rl16(b + 2); cur->srate = (int)rl32(b + 4);
                    cur->block_align = (int)rl16(b + 12); cur->bits = sz >= 16 ? (int)rl16(b + 14) : 8; }
                cur->codec_id = codec_for(cur);
            }
            continue;
        }
        if (!io_read(c, io, NULL, pad)) return -1;   /* avih, strd, strn, JUNK, idx1, ... */
    }
}
/* next chunk of the movi list: 1 = got one (stream index, size; payload unread), 0 = end of file */
static int avi_next(CPU *c, Demux *d, int *sidx, uint32_t *size) {
    Io *io = d->io;
    for (;;) {
        uint8_t ch[8]; if (!io_read(c, io, ch, 8)) return 0;
        uint32_t id = rl32(ch), sz = rl32(ch + 4);
        if (id == TAG('L','I','S','T') || id == TAG('R','I','F','F')) { if (!io_read(c, io, NULL, 4)) return 0; continue; }   /* rec / movi / AVIX: descend */
        int a = ch[0] - '0', b = ch[1] - '0';
        if (a >= 0 && a <= 9 && b >= 0 && b <= 9 && a * 10 + b < d->nst) { *sidx = a * 10 + b; *size = sz; return 1; }
        if (!io_read(c, io, NULL, sz + (sz & 1))) return 0;   /* idx1, JUNK, ix##, unknown */
    }
}

static void lav_destruct_packet(CPU *c);

/* ---------------------------------------------------------------- formats / io */
static uint32_t g_ifmt;   /* the one AVInputFormat we report; only its name is ever looked at */
static void lav_avformat_alloc_context(CPU *c) { c->eax = lavh_alloc(SZ_FMTCTX); }
static void lav_avformat_free_context(CPU *c) { lavh_free(ARG(0)); c->eax = 0; }
static void lav_avio_alloc_context(CPU *c) {
    uint32_t pb = lavh_alloc(SZ_IOCTX); Io *io = calloc(1, sizeof *io); uint32_t g = lavh_alloc(IOBUF);
    if (!pb || !io || !g) { free(io); lavh_free(pb); lavh_free(g); c->eax = 0; return; }
    WR32(pb + IO_BUFFER, ARG(0)); WR32(pb + IO_BUFSIZE, ARG(1)); WR32(pb + IO_WRITEFLAG, ARG(2)); WR32(pb + IO_OPAQUE, ARG(3));
    WR32(pb + IO_READ, ARG(4)); WR32(pb + IO_WRITE, ARG(5)); WR32(pb + IO_SEEK, ARG(6));
    io->o.kind = K_IO; io->pb = pb; io->opaque = ARG(3); io->readfn = ARG(4); io->gbuf = g;
    if (!io->readfn || !obj_put(&io->o)) { free(io); lavh_free(pb); lavh_free(g); c->eax = 0; return; }
    c->eax = pb;
}
static void io_destroy(Io *io) {
    lavh_lock(); for (int i = 1; i < MAXOBJ; i++) if (objs[i] == &io->o) objs[i] = NULL; lavh_unlock();
    lavh_free(io->gbuf); free(io);
}
/* av_probe_input_buffer(pb, &fmt, filename, logctx, offset, max_probe_size) */
static void lav_av_probe_input_buffer(CPU *c) {
    Io *io = io_of(ARG(0));
    if (!io || !io_need(c, io, 12)) { c->eax = INVALID; return; }
    const uint8_t *p = GP(io->gbuf + io->pos);
    if (rl32(p) != TAG('R','I','F','F') || rl32(p + 8) != TAG('A','V','I',' ')) { LOG("not an AVI file; this replacement only plays AVI (put darkrecomp_native_ffmpeg.txt next to the exe to use ffmpeg.dll)"); c->eax = INVALID; return; }
    if (!g_ifmt) { g_ifmt = lavh_alloc(0x80); if (g_ifmt) { WR32(g_ifmt, gstr("avi")); WR32(g_ifmt + 4, gstr("AVI format")); } }
    WR32(ARG(1), g_ifmt); c->eax = 0;
}
static void free_streams(uint32_t ic) {
    uint32_t n = RD32(ic + FC_NB_STREAMS); if (n > FC_MAXSTREAMS) n = FC_MAXSTREAMS;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t st = RD32(ic + FC_STREAMS + 4 * i); if (!st) continue;
        uint32_t cc = RD32(st + ST_CODEC);
        if (cc) { lavh_free(RD32(cc + CC_EXTRADATA)); lavh_free(cc); }
        lavh_free(st); WR32(ic + FC_STREAMS + 4 * i, 0);
    }
    WR32(ic + FC_NB_STREAMS, 0);
}
/* av_open_input_stream(&ic, pb, filename, fmt, ap) */
static void lav_av_open_input_stream(CPU *c) {
    uint32_t icp = ARG(0), pb = ARG(1), ic = RD32(icp); int own = 0;
    Io *io = io_of(pb);
    if (!io) { c->eax = INVALID; return; }
    if (!ic) { ic = lavh_alloc(SZ_FMTCTX); own = 1; if (!ic) { c->eax = ENOMEM_; return; } }
    Demux *d = calloc(1, sizeof *d); uint32_t id = d ? (d->o.kind = K_DEMUX, obj_put(&d->o)) : 0;
    if (!id) { free(d); goto fail; }
    d->io = io; d->ic = ic;
    if (avi_header(c, d) < 0) { LOG("AVI header could not be parsed"); obj_del(id); free(d); goto fail; }
    WR32(ic + FC_IFORMAT, g_ifmt); WR32(ic + FC_PRIV, id); WR32(ic + FC_PB, pb);
    int64_t dur = 0;
    for (int i = 0; i < d->nst; i++) {
        AviStream *s = &d->st[i];
        uint32_t st = lavh_alloc(SZ_STREAM), cc = lavh_alloc(SZ_CODECCTX);
        if (!st || !cc) { lavh_free(st); lavh_free(cc); break; }
        s->gstream = st;
        WR32(st + ST_INDEX, i); WR32(st + ST_ID, i); WR32(st + ST_CODEC, cc);
        WR32(cc + CC_CODECTYPE, s->type < 0 ? (uint32_t)-1 : (uint32_t)s->type); WR32(cc + CC_CODECID, s->codec_id); WR32(cc + CC_CODECTAG, s->tag);
        WR32(cc + CC_PIXFMT, (uint32_t)-1); WR32(cc + CC_SAMPLEFMT, (uint32_t)-1); WR32(st + ST_SAR + 4, 1); WR32(cc + CC_SAR + 4, 1);
        WR64(st + ST_START, 0); WR64(st + ST_CURDTS, 0);
        if (s->type == T_VIDEO) {
            WR32(st + ST_TIMEBASE, s->scale); WR32(st + ST_TIMEBASE + 4, s->rate);
            WR32(st + ST_RFRAMERATE, s->rate); WR32(st + ST_RFRAMERATE + 4, s->scale); WR32(st + ST_AVGFRAMERATE, s->rate); WR32(st + ST_AVGFRAMERATE + 4, s->scale);
            WR32(cc + CC_TIMEBASE, s->scale); WR32(cc + CC_TIMEBASE + 4, s->rate);
            WR32(cc + CC_WIDTH, s->w); WR32(cc + CC_HEIGHT, s->h); WR32(cc + CC_CODEDW, s->w); WR32(cc + CC_CODEDH, s->h);
            WR64(st + ST_DURATION, s->length); WR64(st + ST_NBFRAMES, s->length);
            if (s->codec_id == ID_INDEO5) WR32(cc + CC_PIXFMT, PF_YUV410P);
            if (s->rate) { int64_t us = (int64_t)s->length * s->scale * 1000000 / s->rate; if (us > dur) dur = us; }
        } else if (s->type == T_AUDIO) {
            uint32_t sr = s->srate > 0 ? (uint32_t)s->srate : 22050;
            WR32(st + ST_TIMEBASE, 1); WR32(st + ST_TIMEBASE + 4, sr); WR32(cc + CC_TIMEBASE, 1); WR32(cc + CC_TIMEBASE + 4, sr);
            WR32(cc + CC_SRATE, sr); WR32(cc + CC_CHANNELS, s->channels); WR32(cc + CC_BLOCKALIGN, s->block_align); WR32(cc + CC_BPCS, s->bits);
            WR32(cc + CC_BITRATE, sr * s->channels * s->bits); WR64(st + ST_DURATION, NOPTS);
            if (s->codec_id) WR32(cc + CC_SAMPLEFMT, SF_S16);   /* our PCM decoder always hands out 16-bit samples */
        }
        if (s->extra_len) { uint32_t e = lavh_alloc(s->extra_len + 64); if (e) { memcpy(GP(e), s->extra, s->extra_len); WR32(cc + CC_EXTRADATA, e); WR32(cc + CC_EXTRASIZE, s->extra_len); } }
        WR32(ic + FC_STREAMS + 4 * i, st); WR32(ic + FC_NB_STREAMS, i + 1);
        LOG("stream %d: %s codec %d %dx%d %d Hz %d ch %d-bit, %u/%u%s", i, s->type == T_VIDEO ? "video" : s->type == T_AUDIO ? "audio" : "other",
            s->codec_id, s->w, s->h, s->srate, s->channels, s->bits, s->scale, s->rate, s->codec_id ? "" : " (not supported: ignored)");
    }
    WR64(ic + FC_START, 0); WR64(ic + FC_DURATION, dur ? (uint64_t)dur : NOPTS);
    WR32(icp, ic); c->eax = 0; return;
fail:
    if (own) lavh_free(ic); else { free_streams(ic); lavh_free(ic); }   /* FFmpeg frees a preallocated context on failure, too */
    WR32(icp, 0); c->eax = INVALID;
}
static void demux_close(uint32_t ic) {
    uint32_t id = RD32(ic + FC_PRIV); Demux *d = (Demux *)obj_get(id, K_DEMUX);
    if (d) { obj_del(id); free(d); }
    free_streams(ic);
}
static void lav_av_close_input_stream(CPU *c) {   /* frees the context but not the AVIOContext (the caller owns that) */
    uint32_t ic = ARG(0); if (!ic) return;
    demux_close(ic); lavh_free(ic); c->eax = 0;
}
static void lav_av_close_input_file(CPU *c) {
    uint32_t ic = ARG(0); if (!ic) return;
    uint32_t pb = RD32(ic + FC_PB); demux_close(ic); lavh_free(ic);
    Io *io = io_of(pb); if (io) io_destroy(io);
    lavh_free(pb); c->eax = 0;
}
static void lav_av_find_stream_info(CPU *c) { c->eax = obj_get(RD32(ARG(0) + FC_PRIV), K_DEMUX) ? 0 : INVALID; }
/* av_find_best_stream(ic, type, wanted, related, decoder_ret, flags): first stream of that type we can decode */
static void lav_av_find_best_stream(CPU *c) {
    uint32_t ic = ARG(0); int type = (int)ARG(1), wanted = (int)ARG(2);
    Demux *d = (Demux *)obj_get(RD32(ic + FC_PRIV), K_DEMUX); c->eax = NOTFOUND;
    if (!d) return;
    for (int i = 0; i < d->nst; i++) {
        if (wanted >= 0 && i != wanted) continue;
        if (d->st[i].type == type && d->st[i].codec_id) { c->eax = (uint32_t)i; if (ARG(4)) WR32(ARG(4), 0); return; }
    }
}
/* av_read_frame(ic, pkt) */
static void lav_av_read_frame(CPU *c) {
    uint32_t ic = ARG(0), pkt = ARG(1);
    Demux *d = (Demux *)obj_get(RD32(ic + FC_PRIV), K_DEMUX);
    if (!d) { c->eax = INVALID; return; }
    for (;;) {
        int si; uint32_t sz;
        if (!avi_next(c, d, &si, &sz)) { c->eax = EOF_ERR; return; }
        AviStream *s = &d->st[si]; uint64_t pos = d->io->off - 8; int64_t pts = s->count;
        if (s->type == T_VIDEO) s->count++;
        else if (s->type == T_AUDIO) s->count += s->block_align > 0 ? sz / (uint32_t)s->block_align : sz;
        uint32_t st = s->gstream;
        if (!sz || !st || !s->codec_id || RD32(st + ST_DISCARD) >= 48) { if (!io_read(c, d->io, NULL, sz + (sz & 1))) { c->eax = EOF_ERR; return; } continue; }
        uint32_t data = lavh_alloc(sz + 64);   /* + FFmpeg's input padding (zeroed) */
        if (!data) { c->eax = ENOMEM_; return; }
        if (!io_read(c, d->io, GP(data), sz) || ((sz & 1) && !io_read(c, d->io, NULL, 1))) { lavh_free(data); c->eax = EOF_ERR; return; }
        memset(GP(pkt), 0, SZ_PACKET);
        WR64(pkt + PK_PTS, (uint64_t)pts); WR64(pkt + PK_DTS, (uint64_t)pts); WR32(pkt + PK_DATA, data); WR32(pkt + PK_SIZE, sz);
        WR32(pkt + PK_STREAM, si); WR32(pkt + PK_FLAGS, s->type == T_AUDIO || pts == 0 ? 1 : 0); WR32(pkt + PK_DURATION, s->type == T_VIDEO ? 1 : 0);
        WR32(pkt + PK_DESTRUCT, lavh_fnaddr(lav_destruct_packet)); WR64(pkt + PK_POS, pos); WR64(pkt + PK_CONVDUR, 0);
        c->eax = 0; return;
    }
}

/* ---------------------------------------------------------------- packets, memory, misc */
static void lav_av_init_packet(CPU *c) {
    uint32_t p = ARG(0);
    WR64(p + PK_PTS, NOPTS); WR64(p + PK_DTS, NOPTS); WR64(p + PK_POS, (uint64_t)-1); WR32(p + PK_DURATION, 0); WR64(p + PK_CONVDUR, 0);
    WR32(p + PK_FLAGS, 0); WR32(p + PK_STREAM, 0); WR32(p + PK_DESTRUCT, 0); WR32(p + PK_PRIV, 0);
}
static void lav_av_free_packet(CPU *c) {
    uint32_t p = ARG(0); if (!p) return;
    if (RD32(p + PK_DESTRUCT)) lavh_free(RD32(p + PK_DATA));
    WR32(p + PK_DATA, 0); WR32(p + PK_SIZE, 0); WR32(p + PK_DESTRUCT, 0);
}
static void lav_destruct_packet(CPU *c) { lav_av_free_packet(c); }   /* AVPacket.destruct of our packets (lgvid never calls it itself) */
static void lav_av_dup_packet(CPU *c) {   /* our packets always own their data; a borrowed one gets a private copy */
    uint32_t p = ARG(0); c->eax = 0;
    if (RD32(p + PK_DESTRUCT) || !RD32(p + PK_DATA)) return;
    uint32_t sz = RD32(p + PK_SIZE), d = lavh_alloc(sz + 64); if (!d) { c->eax = ENOMEM_; return; }
    memcpy(GP(d), GP(RD32(p + PK_DATA)), sz); WR32(p + PK_DATA, d); WR32(p + PK_DESTRUCT, lavh_fnaddr(lav_destruct_packet));
}
static void lav_av_malloc(CPU *c) { c->eax = ARG(0) < 0x7fffffff ? lavh_alloc(ARG(0) ? ARG(0) : 1) : 0; }
static void lav_av_free(CPU *c) {
    uint32_t p = ARG(0); if (!p) return;
    Io *io = io_of(p); if (io) io_destroy(io);   /* lgvid frees its AVIOContext with av_free */
    lavh_free(p);
}
static void lav_av_freep(CPU *c) { uint32_t pp = ARG(0); uint32_t p = RD32(pp); if (p) { Io *io = io_of(p); if (io) io_destroy(io); lavh_free(p); } WR32(pp, 0); }
static void lav_av_gettime(CPU *c) { uint64_t t = (uint64_t)lavh_time_us(); c->eax = (uint32_t)t; c->edx = (uint32_t)(t >> 32); }
static void lav_nop(CPU *c) { c->eax = 0; }
static void lav_av_get_bits_per_sample_fmt(CPU *c) { static const int b[] = { 8, 16, 32, 32, 64 }; uint32_t f = ARG(0); c->eax = f < 5 ? (uint32_t)b[f] : 0; }
static void lav_av_get_sample_fmt_name(CPU *c) {
    static uint32_t nm[5]; static const char *s[] = { "u8", "s16", "s32", "flt", "dbl" }; uint32_t f = ARG(0);
    if (f >= 5) { c->eax = 0; return; }
    if (!nm[f]) nm[f] = gstr(s[f]); c->eax = nm[f];
}

/* ---------------------------------------------------------------- decoders */
typedef struct { Obj o; int id; AVCodecContext hctx; AVFrame hf; uint8_t *pad; uint32_t padsz; uint32_t gplanes, gsize; } Dec;
static uint32_t g_codec[2];
/* avcodec_find_decoder(id) */
static void lav_avcodec_find_decoder(CPU *c) {
    uint32_t id = ARG(0); int k = id == ID_INDEO5 ? 0 : (id == ID_PCM_S16LE || id == ID_PCM_U8) ? 1 : -1;
    if (k < 0) { c->eax = 0; return; }
    if (!g_codec[k]) {   /* name, type and id are all anyone reads */
        uint32_t g = lavh_alloc(SZ_CODEC); if (!g) { c->eax = 0; return; }
        WR32(g + CO_NAME, gstr(k ? "pcm" : "indeo5")); WR32(g + CO_TYPE, k ? T_AUDIO : T_VIDEO); WR32(g + CO_ID, k ? ID_PCM_S16LE : ID_INDEO5);
        g_codec[k] = g;
    }
    c->eax = g_codec[k];
}
/* avcodec_open(avctx, codec) */
static void lav_avcodec_open(CPU *c) {
    uint32_t cc = ARG(0), codec = ARG(1); int id = (int)RD32(cc + CC_CODECID);
    if (!codec || obj_get(RD32(cc + CC_PRIV), K_DEC)) { c->eax = (uint32_t)-1; return; }
    Dec *d = calloc(1, sizeof *d); if (!d) { c->eax = ENOMEM_; return; }
    d->o.kind = K_DEC; d->id = id;
    if (id == ID_INDEO5) {
        d->hctx.width = d->hctx.coded_width = (int)RD32(cc + CC_WIDTH); d->hctx.height = d->hctx.coded_height = (int)RD32(cc + CC_HEIGHT);
        d->hctx.codec_id = AV_CODEC_ID_INDEO5;
        d->hctx.priv_data = av_mallocz(ff_indeo5_decoder.priv_data_size);
        if (!d->hctx.priv_data || ff_indeo5_decoder.init(&d->hctx) < 0) { LOG("Indeo 5 decoder init failed"); av_free(d->hctx.priv_data); free(d); c->eax = (uint32_t)-1; return; }
        WR32(cc + CC_PIXFMT, PF_YUV410P);
    } else if (id == ID_PCM_S16LE || id == ID_PCM_U8) {
        WR32(cc + CC_SAMPLEFMT, SF_S16);
    } else { free(d); c->eax = (uint32_t)-1; return; }
    uint32_t h = obj_put(&d->o);
    if (!h) { if (id == ID_INDEO5) { ff_indeo5_decoder.close(&d->hctx); av_free(d->hctx.priv_data); } free(d); c->eax = (uint32_t)-1; return; }
    WR32(cc + CC_PRIV, h); WR32(cc + CC_CODEC, codec); c->eax = 0;
}
static void lav_avcodec_close(CPU *c) {
    uint32_t cc = ARG(0), h = RD32(cc + CC_PRIV); Dec *d = (Dec *)obj_get(h, K_DEC); c->eax = 0;
    if (!d) return;
    obj_del(h); WR32(cc + CC_PRIV, 0); WR32(cc + CC_CODEC, 0);
    if (d->id == ID_INDEO5) { av_frame_unref(&d->hf); ff_indeo5_decoder.close(&d->hctx); av_free(d->hctx.priv_data); }
    lavh_free(d->gplanes); free(d->pad); free(d);
}
static void lav_avcodec_alloc_frame(CPU *c) {
    uint32_t f = lavh_alloc(SZ_FRAME); c->eax = f; if (!f) return;
    WR64(f + FR_PTS, NOPTS); WR32(f + FR_KEY, 1); WR64(f + FR_BEST, NOPTS); WR64(f + FR_PKTPTS, NOPTS); WR64(f + FR_PKTDTS, NOPTS); WR64(f + FR_PKTPOS, (uint64_t)-1);
}
/* avcodec_decode_video2(avctx, frame, &got_picture, pkt) */
static void lav_avcodec_decode_video2(CPU *c) {
    uint32_t cc = ARG(0), fr = ARG(1), gotp = ARG(2), pkt = ARG(3);
    Dec *d = (Dec *)obj_get(RD32(cc + CC_PRIV), K_DEC); WR32(gotp, 0);
    if (!d || d->id != ID_INDEO5) { c->eax = (uint32_t)-1; return; }
    uint32_t sz = RD32(pkt + PK_SIZE), data = RD32(pkt + PK_DATA);
    if (!sz || !data) { c->eax = 0; return; }
    if (d->padsz < sz + 64) { free(d->pad); d->pad = malloc(sz + 64); d->padsz = d->pad ? sz + 64 : 0; if (!d->pad) { c->eax = ENOMEM_; return; } }
    memcpy(d->pad, GP(data), sz); memset(d->pad + sz, 0, 64);
    AVPacket hp = { d->pad, (int)sz }; int got = 0;
    av_frame_unref(&d->hf);
    int r = ff_indeo5_decoder.cb.decode(&d->hctx, &d->hf, &got, &hp);
    if (r < 0) { c->eax = (uint32_t)r; return; }
    if (got) {
        int w = d->hctx.width, h = d->hctx.height, ch = (h + 3) / 4;
        uint32_t ls[3] = { (uint32_t)d->hf.linesize[0], (uint32_t)d->hf.linesize[1], (uint32_t)d->hf.linesize[2] };
        uint32_t need = ls[0] * h + (ls[1] + ls[2]) * ch + 64;
        if (d->gsize < need) { lavh_free(d->gplanes); d->gplanes = lavh_alloc(need); d->gsize = d->gplanes ? need : 0; if (!d->gplanes) { c->eax = ENOMEM_; return; } }
        uint32_t g0 = d->gplanes, g1 = g0 + ls[0] * h, g2 = g1 + ls[1] * ch;
        memcpy(GP(g0), d->hf.data[0], (size_t)ls[0] * h); memcpy(GP(g1), d->hf.data[1], (size_t)ls[1] * ch); memcpy(GP(g2), d->hf.data[2], (size_t)ls[2] * ch);
        WR32(fr + FR_DATA, g0); WR32(fr + FR_DATA + 4, g1); WR32(fr + FR_DATA + 8, g2); WR32(fr + FR_DATA + 12, 0);
        WR32(fr + FR_LINESIZE, ls[0]); WR32(fr + FR_LINESIZE + 4, ls[1]); WR32(fr + FR_LINESIZE + 8, ls[2]); WR32(fr + FR_LINESIZE + 12, 0);
        uint64_t pts = RD64(pkt + PK_PTS);
        WR32(fr + FR_KEY, 1); WR32(fr + FR_PICTTYPE, 1); WR32(fr + FR_REPEAT, 0);
        WR64(fr + FR_PTS, pts); WR64(fr + FR_PKTPTS, pts); WR64(fr + FR_PKTDTS, RD64(pkt + PK_DTS)); WR64(fr + FR_BEST, pts); WR64(fr + FR_PKTPOS, RD64(pkt + PK_POS));
        WR32(cc + CC_WIDTH, w); WR32(cc + CC_HEIGHT, h); WR32(cc + CC_PIXFMT, PF_YUV410P);
        WR32(gotp, 1);
    }
    c->eax = sz;
}
/* avcodec_decode_audio3(avctx, int16_t *samples, int *frame_size_ptr (in: buffer bytes, out: bytes written), pkt) */
static void lav_avcodec_decode_audio3(CPU *c) {
    uint32_t cc = ARG(0), out = ARG(1), fsp = ARG(2), pkt = ARG(3);
    Dec *d = (Dec *)obj_get(RD32(cc + CC_PRIV), K_DEC);
    uint32_t room = RD32(fsp), sz = RD32(pkt + PK_SIZE), data = RD32(pkt + PK_DATA); WR32(fsp, 0);
    if (!d || (d->id != ID_PCM_S16LE && d->id != ID_PCM_U8)) { c->eax = (uint32_t)-1; return; }
    if (!sz || !data) { c->eax = 0; return; }
    uint32_t ba = RD32(cc + CC_BLOCKALIGN); if (!ba) ba = 1;
    if (d->id == ID_PCM_S16LE) {
        uint32_t n = sz < room ? sz : room;
        if (n >= ba) n -= n % ba;
        n &= ~1u; if (!n) { c->eax = sz; return; }   /* never report "nothing consumed" for a non-empty packet: lgvid would spin */
        memcpy(GP(out), GP(data), n); WR32(fsp, n); c->eax = n;
    } else {
        uint32_t n = sz < room / 2 ? sz : room / 2; if (!n) { c->eax = sz; return; }
        const uint8_t *s = GP(data); uint8_t *o = GP(out);
        for (uint32_t i = 0; i < n; i++) { int16_t v = (int16_t)((s[i] - 128) << 8); memcpy(o + 2 * i, &v, 2); }
        WR32(fsp, 2 * n); c->eax = n;
    }
}

/* ---------------------------------------------------------------- audio sample format conversion (to 16-bit) */
typedef struct { Obj o; int out_fmt, in_fmt; } Conv;
static void lav_av_audio_convert_alloc(CPU *c) {   /* (out_fmt, out_channels, in_fmt, in_channels, matrix, flags) */
    Conv *v = calloc(1, sizeof *v); uint32_t g = lavh_alloc(16), h;
    if (!v || !g || !(h = (v->o.kind = K_CONV, v->out_fmt = (int)ARG(0), v->in_fmt = (int)ARG(2), obj_put(&v->o)))) { free(v); lavh_free(g); c->eax = 0; return; }
    WR32(g, 0x4c41564b); WR32(g + 4, h); c->eax = g;
}
static void lav_av_audio_convert_free(CPU *c) {
    uint32_t g = ARG(0); if (!g) return;
    Conv *v = (Conv *)obj_get(RD32(g + 4), K_CONV); if (v) { obj_del(RD32(g + 4)); free(v); }
    lavh_free(g);
}
static double samp_in(const uint8_t *p, int f) {
    switch (f) {
    case SF_U8: return (p[0] - 128) / 128.0;
    case SF_S16: { int16_t v; memcpy(&v, p, 2); return v / 32768.0; }
    case SF_S32: { int32_t v; memcpy(&v, p, 4); return v / 2147483648.0; }
    case SF_FLT: { float v; memcpy(&v, p, 4); return v; }
    case SF_DBL: { double v; memcpy(&v, p, 8); return v; }
    }
    return 0;
}
/* av_audio_convert(ctx, out[6], out_stride[6], in[6], in_stride[6], len): lgvid passes one interleaved "channel" */
static void lav_av_audio_convert(CPU *c) {
    uint32_t g = ARG(0); Conv *v = g ? (Conv *)obj_get(RD32(g + 4), K_CONV) : NULL; c->eax = (uint32_t)-1;
    if (!v) return;
    for (int ch = 0; ch < 6; ch++) {
        uint32_t o = RD32(ARG(1) + 4 * ch), i = RD32(ARG(3) + 4 * ch); if (!o || !i) continue;
        int os = (int)RD32(ARG(2) + 4 * ch), is = (int)RD32(ARG(4) + 4 * ch), len = (int)ARG(5);
        for (int k = 0; k < len; k++) {
            double s = samp_in(GP(i + (uint32_t)(k * is)), v->in_fmt) * 32768.0;
            int iv = s >= 32767.0 ? 32767 : s <= -32768.0 ? -32768 : (int)(s < 0 ? s - 0.5 : s + 0.5);
            int16_t sv = (int16_t)iv; memcpy(GP(o + (uint32_t)(k * os)), &sv, 2);
        }
    }
    c->eax = 0;
}

/* ---------------------------------------------------------------- YUV -> RGB with scaling (sws_*) */
typedef struct { int n, *idx; int16_t *w; } Filt;   /* per output sample: n taps starting at idx[i]..., weights sum to 1<<14 */
typedef struct {
    Obj o; int sw, sh, sf, dw, dh, df, flags, cx, cy;
    Filt fy_h, fy_v, fc_h, fc_v; int32_t *ty, *tu, *tv; uint8_t *ly, *lu, *lv;
} Sws;
static double cubic(double x) { const double a = -0.6; x = x < 0 ? -x : x;   /* Keys kernel, FFmpeg's default bicubic (B=0, C=0.6) */
    return x < 1 ? ((a + 2) * x - (a + 3)) * x * x + 1 : x < 2 ? ((a * x - 5 * a) * x + 8 * a) * x - 4 * a : 0; }
static int filt_make(Filt *f, int src, int dst, int shift, int kind) {   /* kind: 1 point, 2 bilinear, 4 bicubic */
    int srcp = (src + (1 << shift) - 1) >> shift;
    f->n = kind; f->idx = malloc(sizeof(int) * dst * kind); f->w = malloc(sizeof(int16_t) * dst * kind);
    if (!f->idx || !f->w) return 0;
    for (int i = 0; i < dst; i++) {
        double x = ((i + 0.5) * src / dst) / (1 << shift) - 0.5; int b = (int)(x < 0 ? x - 1 : x); double fr = x - b; double w[4];
        if (kind == 1) { b = (int)(x + 0.5); w[0] = 1; }
        else if (kind == 2) { w[0] = 1 - fr; w[1] = fr; }
        else { b -= 1; for (int k = 0; k < 4; k++) w[k] = cubic(fr + 1 - k); }
        int sum = 0;
        for (int k = 0; k < kind; k++) {
            int s = b + k; s = s < 0 ? 0 : s >= srcp ? srcp - 1 : s;
            int wi = (int)(w[k] * 16384 + (w[k] < 0 ? -0.5 : 0.5)); if (k == kind - 1) wi = 16384 - sum; sum += wi;
            f->idx[i * kind + k] = s; f->w[i * kind + k] = (int16_t)wi;
        }
    }
    return 1;
}
static void filt_free(Filt *f) { free(f->idx); free(f->w); }
static void sws_destroy(Sws *s) {
    filt_free(&s->fy_h); filt_free(&s->fy_v); filt_free(&s->fc_h); filt_free(&s->fc_v);
    free(s->ty); free(s->tu); free(s->tv); free(s->ly); free(s->lu); free(s->lv); free(s);
}
static int fmt_ok_src(int f) { return f == PF_YUV410P || f == PF_YUV420P; }
static int fmt_bpp(int f) {
    switch (f) { case PF_ARGB: case PF_RGBA: case PF_ABGR: case PF_BGRA: return 4; case PF_RGB565LE: case PF_BGR565LE: return 2; case PF_RGB24: case PF_BGR24: return 3; }
    return 0;
}
/* sws_getCachedContext(ctx, srcW, srcH, srcFormat, dstW, dstH, dstFormat, flags, srcFilter, dstFilter, param) */
static void lav_sws_getCachedContext(CPU *c) {
    uint32_t g = ARG(0); int sw = (int)ARG(1), sh = (int)ARG(2), sf = (int)ARG(3), dw = (int)ARG(4), dh = (int)ARG(5), df = (int)ARG(6), fl = (int)ARG(7);
    if (g) {
        Sws *s = (Sws *)obj_get(RD32(g + 4), K_SWS);
        if (s && s->sw == sw && s->sh == sh && s->sf == sf && s->dw == dw && s->dh == dh && s->df == df && s->flags == fl) { c->eax = g; return; }
        if (s) { obj_del(RD32(g + 4)); sws_destroy(s); }
        lavh_free(g);
    }
    c->eax = 0;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0 || sw > 8192 || sh > 8192 || dw > 16384 || dh > 16384 || !fmt_ok_src(sf) || !fmt_bpp(df)) {
        LOG("sws_getCachedContext: unsupported conversion %dx%d fmt %d -> %dx%d fmt %d", sw, sh, sf, dw, dh, df); return;
    }
    Sws *s = calloc(1, sizeof *s); if (!s) return;
    s->o.kind = K_SWS; s->sw = sw; s->sh = sh; s->sf = sf; s->dw = dw; s->dh = dh; s->df = df; s->flags = fl;
    s->cx = s->cy = sf == PF_YUV410P ? 2 : 1;
    int ky = (fl & SWS_POINT) ? 1 : (fl & (SWS_FAST_BILINEAR | SWS_BILINEAR | SWS_AREA)) ? 2 : 4, kc = ky == 1 ? 1 : 2;
    int cw = (sw + (1 << s->cx) - 1) >> s->cx, chh = (sh + (1 << s->cy) - 1) >> s->cy;
    int ok = filt_make(&s->fy_h, sw, dw, 0, ky) && filt_make(&s->fy_v, sh, dh, 0, ky) && filt_make(&s->fc_h, sw, dw, s->cx, kc) && filt_make(&s->fc_v, sh, dh, s->cy, kc);
    s->ty = malloc(sizeof(int32_t) * dw * sh); s->tu = malloc(sizeof(int32_t) * dw * chh); s->tv = malloc(sizeof(int32_t) * dw * chh);
    s->ly = malloc(dw); s->lu = malloc(dw); s->lv = malloc(dw);
    uint32_t h = 0;
    if (!ok || !s->ty || !s->tu || !s->tv || !s->ly || !s->lu || !s->lv || !(g = lavh_alloc(16)) || !(h = obj_put(&s->o))) { lavh_free(g); sws_destroy(s); return; }
    (void)cw;
    WR32(g, 0x4c535753); WR32(g + 4, h); c->eax = g;
    LOG("scaler: %dx%d fmt %d -> %dx%d fmt %d, %s", sw, sh, sf, dw, dh, df, ky == 1 ? "point" : ky == 2 ? "bilinear" : "bicubic");
}
static void lav_sws_freeContext(CPU *c) {
    uint32_t g = ARG(0); if (!g) return;
    Sws *s = (Sws *)obj_get(RD32(g + 4), K_SWS); if (s) { obj_del(RD32(g + 4)); sws_destroy(s); }
    lavh_free(g);
}
static void hpass(const Filt *f, const uint8_t *src, int32_t *dst, int dw) {   /* one row: 8-bit -> value << 8 (14-bit weights, >> 6) */
    const int n = f->n; const int *ix = f->idx; const int16_t *w = f->w;
    if (n == 1) for (int x = 0; x < dw; x++) dst[x] = src[ix[x]] << 8;
    else if (n == 2) for (int x = 0; x < dw; x++) dst[x] = (src[ix[2 * x]] * w[2 * x] + src[ix[2 * x + 1]] * w[2 * x + 1] + 32) >> 6;
    else for (int x = 0; x < dw; x++) { const int *i = ix + 4 * x; const int16_t *v = w + 4 * x; dst[x] = (src[i[0]] * v[0] + src[i[1]] * v[1] + src[i[2]] * v[2] + src[i[3]] * v[3] + 32) >> 6; }
}
static void vpass(const Filt *f, int y, const int32_t *t, int dw, uint8_t *out) {   /* one output row from horizontally filtered rows (fits int32: |v| < 2^17, weights < 2^15) */
    const int n = f->n; const int *ix = f->idx + n * y; const int16_t *w = f->w + n * y;
    if (n == 1) { const int32_t *a = t + (size_t)ix[0] * dw; for (int x = 0; x < dw; x++) { int v = (a[x] + 128) >> 8; out[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); } return; }
    if (n == 2) { const int32_t *a = t + (size_t)ix[0] * dw, *b = t + (size_t)ix[1] * dw; const int wa = w[0], wb = w[1];
        for (int x = 0; x < dw; x++) { int v = (a[x] * wa + b[x] * wb + (1 << 21)) >> 22; out[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); } return; }
    const int32_t *a = t + (size_t)ix[0] * dw, *b = t + (size_t)ix[1] * dw, *cc = t + (size_t)ix[2] * dw, *d = t + (size_t)ix[3] * dw; const int w0 = w[0], w1 = w[1], w2 = w[2], w3 = w[3];
    for (int x = 0; x < dw; x++) { int v = (a[x] * w0 + b[x] * w1 + cc[x] * w2 + d[x] * w3 + (1 << 21)) >> 22; out[x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
}
static inline uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
/* sws_scale(ctx, srcSlice[], srcStride[], srcSliceY, srcSliceH, dst[], dstStride[]) */
static void lav_sws_scale(CPU *c) {
    uint32_t g = ARG(0); Sws *s = g ? (Sws *)obj_get(RD32(g + 4), K_SWS) : NULL; c->eax = 0;
    if (!s || ARG(3) != 0) return;
    uint32_t sp[3], ss[3]; for (int i = 0; i < 3; i++) { sp[i] = RD32(ARG(1) + 4 * i); ss[i] = RD32(ARG(2) + 4 * i); }
    uint32_t dp = RD32(ARG(5)); int ds = (int)RD32(ARG(6));
    if (!sp[0] || !sp[1] || !sp[2] || !dp) return;
    int sh = s->sh, chh = (sh + (1 << s->cy) - 1) >> s->cy, dw = s->dw, bpp = fmt_bpp(s->df);
    for (int y = 0; y < sh; y++) hpass(&s->fy_h, GP(sp[0] + ss[0] * (uint32_t)y), s->ty + (size_t)y * dw, dw);
    for (int y = 0; y < chh; y++) { hpass(&s->fc_h, GP(sp[1] + ss[1] * (uint32_t)y), s->tu + (size_t)y * dw, dw); hpass(&s->fc_h, GP(sp[2] + ss[2] * (uint32_t)y), s->tv + (size_t)y * dw, dw); }
    for (int y = 0; y < s->dh; y++) {
        vpass(&s->fy_v, y, s->ty, dw, s->ly); vpass(&s->fc_v, y, s->tu, dw, s->lu); vpass(&s->fc_v, y, s->tv, dw, s->lv);
        uint8_t *o = GP(dp + (uint32_t)(ds * y)); const uint8_t *Y = s->ly, *U = s->lu, *V = s->lv;
        /* BT.601, video range, as FFmpeg's swscale does by default */
#define PIX_LOOP(BODY) for (int x = 0; x < dw; x++, o += bpp) { int C = 298 * (Y[x] - 16), D = U[x] - 128, E = V[x] - 128; \
            uint8_t r = clip8((C + 409 * E + 128) >> 8), gg = clip8((C - 100 * D - 208 * E + 128) >> 8), b = clip8((C + 516 * D + 128) >> 8); BODY; }
        switch (s->df) {
        case PF_BGRA: PIX_LOOP(o[0] = b; o[1] = gg; o[2] = r; o[3] = 255) break;
        case PF_RGBA: PIX_LOOP(o[0] = r; o[1] = gg; o[2] = b; o[3] = 255) break;
        case PF_ARGB: PIX_LOOP(o[0] = 255; o[1] = r; o[2] = gg; o[3] = b) break;
        case PF_ABGR: PIX_LOOP(o[0] = 255; o[1] = b; o[2] = gg; o[3] = r) break;
        case PF_RGB24: PIX_LOOP(o[0] = r; o[1] = gg; o[2] = b) break;
        case PF_BGR24: PIX_LOOP(o[0] = b; o[1] = gg; o[2] = r) break;
        case PF_RGB565LE: PIX_LOOP(uint16_t v = (uint16_t)((r >> 3) << 11 | (gg >> 2) << 5 | b >> 3); o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8)) break;
        case PF_BGR565LE: PIX_LOOP(uint16_t v = (uint16_t)((b >> 3) << 11 | (gg >> 2) << 5 | r >> 3); o[0] = (uint8_t)v; o[1] = (uint8_t)(v >> 8)) break;
        }
#undef PIX_LOOP
    }
    c->eax = (uint32_t)s->dh;
}

/* ---------------------------------------------------------------- unused by lgvid but resolved by it: harmless stand-ins */
static void lav_unused(CPU *c) { LOG("an ffmpeg function lgvid.dll was not expected to call was called; returning an error"); c->eax = (uint32_t)-1; }

const LavExport lav_exports[] = {
    { "av_malloc", lav_av_malloc }, { "av_freep", lav_av_freep }, { "av_gettime", lav_av_gettime }, { "av_free_packet", lav_av_free_packet },
    { "av_read_frame", lav_av_read_frame }, { "av_init_packet", lav_av_init_packet }, { "av_dup_packet", lav_av_dup_packet }, { "av_free", lav_av_free },
    { "av_get_bits_per_sample_fmt", lav_av_get_bits_per_sample_fmt }, { "av_get_sample_fmt_name", lav_av_get_sample_fmt_name },
    { "av_register_all", lav_nop }, { "av_close_input_file", lav_av_close_input_file }, { "av_find_stream_info", lav_av_find_stream_info },
    { "av_find_best_stream", lav_av_find_best_stream }, { "av_open_input_stream", lav_av_open_input_stream },
    { "av_close_input_stream", lav_av_close_input_stream }, { "av_probe_input_buffer", lav_av_probe_input_buffer },
    { "av_audio_convert", lav_av_audio_convert }, { "av_log_set_callback", lav_nop }, { "av_log_set_level", lav_nop }, { "dump_format", lav_nop },
    { "avformat_alloc_context", lav_avformat_alloc_context }, { "avformat_free_context", lav_avformat_free_context },
    { "av_audio_convert_alloc", lav_av_audio_convert_alloc }, { "av_audio_convert_free", lav_av_audio_convert_free },
    /* avcodec_enable_ir50dll deliberately absent: lgvid then never tries the Windows Indeo codec (ir50_32.dll) */
    { "avcodec_init", lav_nop }, { "avcodec_default_get_buffer", lav_unused }, { "avcodec_default_release_buffer", lav_unused },
    { "avcodec_decode_video2", lav_avcodec_decode_video2 }, { "avcodec_alloc_frame", lav_avcodec_alloc_frame }, { "avcodec_open", lav_avcodec_open },
    { "avcodec_find_decoder", lav_avcodec_find_decoder }, { "avcodec_decode_audio3", lav_avcodec_decode_audio3 }, { "avcodec_close", lav_avcodec_close },
    { "sws_scale", lav_sws_scale }, { "sws_freeContext", lav_sws_freeContext }, { "sws_getCachedContext", lav_sws_getCachedContext },
    { "avio_alloc_context", lav_avio_alloc_context },
    { 0, 0 }
};
