/*
 * ZSX DCT video, a deliberately small native YUV420P codec.
 *
 * This is not the historic ZSV1 Python stream and does not use its packet
 * syntax.  Transform arithmetic is entirely integer so packets and reference
 * pictures are bit-identical on Windows and Unix C implementations.
 */
#include "config.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "internal.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "libavutil/common.h"
#include "libavutil/crc.h"
#include "libavutil/error.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"

#include "zsx_coefficient_rle.h"
#include "zsx_core_api.h"

#define ZSXD_VERSION          ZSX_VIDEO_STREAM
#define ZSXD_EXTRA            16
#define ZSXD_HEADER           32
#define ZSXD_P0               1
#define ZSXD_MAX_DIMENSION    4096
#define ZSXD_MAX_BLOCKS       (1024U * 1024U)
#define ZSXD_MAX_RLE          (128U * 1024U * 1024U)
#define ZSXD_CORE_CHUNK       (12U * 1024U * 1024U)
#define ZSXD_BASIS_SCALE      16384
#define ZSXD_BASIS_SQUARE     INT64_C(268435456)
#define ZSXD_KEY_INTERVAL     24

typedef struct ZSXDDCTContext {
    const AVClass *class;
    int quality;
    int entropy_profile;
    int entropy_version;
    int width;
    int height;
    uint8_t *reference;
    size_t reference_size;
    uint32_t next_index;
    int have_reference;
} ZSXDDCTContext;

#define OFFSET(x) offsetof(ZSXDDCTContext, x)
#define VE AV_OPT_FLAG_VIDEO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption zsxd_options[] = {
    { "zsx_quality", "JPEG quantisation quality", OFFSET(quality),
      AV_OPT_TYPE_INT, { .i64 = 70 }, 1, 100, VE },
    { "zsx_entropy", "Entropy profile: 0=size trials, 1=byte16, 2=byte20, 3=candidate16, 4=candidate20",
      OFFSET(entropy_profile), AV_OPT_TYPE_INT, { .i64 = 0 }, 0, 4, VE },
    { NULL },
};

static const AVClass zsxd_class = {
    .class_name = "zsx_dct_video",
    .item_name  = av_default_item_name,
    .option     = zsxd_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const enum AVPixelFormat zsxd_formats[] = {
    AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE,
};

/*
 * Orthonormal DCT-II basis, rounded once to 14 fractional bits.  These
 * literals are intentionally not generated with cos(): libm differences
 * would otherwise alter a reference picture and its CRC.
 */
static const int16_t zsxd_basis[8][8] = {
    { 5793,  5793,  5793,  5793,  5793,  5793,  5793,  5793 },
    { 8035,  6811,  4551,  1598, -1598, -4551, -6811, -8035 },
    { 7568,  3135, -3135, -7568, -7568, -3135,  3135,  7568 },
    { 6811, -1598, -8035, -4551,  4551,  8035,  1598, -6811 },
    { 5793, -5793, -5793,  5793,  5793, -5793, -5793,  5793 },
    { 4551, -8035,  1598,  6811, -6811, -1598,  8035, -4551 },
    { 3135, -7568,  7568, -3135, -3135,  7568, -7568,  3135 },
    { 1598, -4551,  6811, -8035,  8035, -6811,  4551, -1598 },
};

static const uint8_t zsxd_q_luma[64] = {
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55,
    14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92,
    49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99
};
static const uint8_t zsxd_q_chroma[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99,
    24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
};

static uint32_t zsxd_crc(const uint8_t *data, size_t size)
{
    return av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), 0, data, size);
}

/* Symmetric nearest-integer division; denominator is always positive. */
static int64_t zsxd_round_div(int64_t value, int64_t denominator)
{
    if (value < 0)
        return -((-value + denominator / 2) / denominator);
    return (value + denominator / 2) / denominator;
}

static int zsxd_geometry(int width, int height, size_t counts[3],
                         size_t offsets[3], size_t *pixels,
                         size_t *coefficient_count)
{
    int plane;
    size_t p = 0, c = 0;
    if (width <= 0 || height <= 0 || width > ZSXD_MAX_DIMENSION ||
        height > ZSXD_MAX_DIMENSION || (width & 1) || (height & 1))
        return AVERROR(EINVAL);
    for (plane = 0; plane < 3; plane++) {
        int pw = plane ? width / 2 : width;
        int ph = plane ? height / 2 : height;
        size_t bw = (pw + 7) / 8, bh = (ph + 7) / 8;
        if (bw > SIZE_MAX / bh || bw * bh > ZSXD_MAX_BLOCKS)
            return AVERROR(EOVERFLOW);
        counts[plane] = bw * bh;
        offsets[plane] = p;
        if ((size_t)pw > SIZE_MAX / ph || p > SIZE_MAX - (size_t)pw * ph ||
            c > SIZE_MAX - counts[plane] * 64)
            return AVERROR(EOVERFLOW);
        p += (size_t)pw * ph;
        c += counts[plane] * 64;
    }
    *pixels = p;
    *coefficient_count = c;
    return 0;
}

static void zsxd_quant_table(int quality, int plane, uint16_t q[64])
{
    const uint8_t *base = plane ? zsxd_q_chroma : zsxd_q_luma;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    int i;
    for (i = 0; i < 64; i++)
        q[i] = av_clip((base[i] * scale + 50) / 100, 1, 255);
}

static void zsxd_forward_block(const uint8_t *source, const uint8_t *ref,
                               int width, int height, int bx, int by,
                               int intra, const uint16_t q[64], int16_t *out)
{
    int64_t tmp[8][8];
    int x, y, u, v;
    for (y = 0; y < 8; y++) for (u = 0; u < 8; u++) {
        int64_t sum = 0;
        int sy = FFMIN(by * 8 + y, height - 1);
        for (x = 0; x < 8; x++) {
            int sx = FFMIN(bx * 8 + x, width - 1);
            int value = source[(size_t)sy * width + sx];
            if (intra)
                value -= 128;
            else
                value -= ref[(size_t)sy * width + sx];
            sum += (int64_t)zsxd_basis[u][x] * value;
        }
        tmp[y][u] = sum;
    }
    for (v = 0; v < 8; v++) for (u = 0; u < 8; u++) {
        int64_t sum = 0, value;
        for (y = 0; y < 8; y++)
            sum += (int64_t)zsxd_basis[v][y] * tmp[y][u];
        value = zsxd_round_div(sum, ZSXD_BASIS_SQUARE * q[v * 8 + u]);
        /* This is also the wire coefficient range enforced by the RLE parser. */
        out[v * 8 + u] = av_clip64(value, INT16_MIN, INT16_MAX);
    }
}

static void zsxd_inverse_block(const int16_t *coeff, const uint16_t q[64],
                               uint8_t *recon, const uint8_t *ref,
                               int width, int height, int bx, int by,
                               int intra)
{
    int64_t tmp[8][8];
    int x, y, u, v;
    /*
     * No intermediate division is performed: coeff*Q and both basis sums
     * remain int64, followed by the exact 16384^2 denominator once.
     */
    for (v = 0; v < 8; v++) for (x = 0; x < 8; x++) {
        int64_t sum = 0;
        for (u = 0; u < 8; u++)
            sum += (int64_t)zsxd_basis[u][x] * coeff[v * 8 + u] *
                   q[v * 8 + u];
        tmp[v][x] = sum;
    }
    for (y = 0; y < 8; y++) for (x = 0; x < 8; x++) {
        int64_t sum = 0, value;
        int sy = by * 8 + y, sx = bx * 8 + x;
        if (sy >= height || sx >= width)
            continue;
        for (v = 0; v < 8; v++)
            sum += (int64_t)zsxd_basis[v][y] * tmp[v][x];
        value = zsxd_round_div(sum, ZSXD_BASIS_SQUARE);
        if (intra)
            value += 128;
        else
            value += ref[(size_t)sy * width + sx];
        recon[(size_t)sy * width + sx] = (uint8_t)av_clip64(value, 0, 255);
    }
}

static int zsxd_rle_from_picture(ZSXDDCTContext *s, const uint8_t *source,
                                 const uint8_t *reference, int intra,
                                 uint8_t *recon, uint8_t **raw,
                                 size_t *raw_size, uint64_t *sse)
{
    size_t counts[3], offsets[3], pixels, coefficient_count, coeff_offset = 0;
    const int16_t *planes[3];
    uint16_t q[3][64];
    int16_t *coeff = NULL;
    uint8_t *rle = NULL;
    int plane, ret;
    uint64_t error = 0;

    ret = zsxd_geometry(s->width, s->height, counts, offsets,
                        &pixels, &coefficient_count);
    if (ret < 0 || pixels != s->reference_size)
        return ret < 0 ? ret : AVERROR_BUG;
    if (coefficient_count > SIZE_MAX / sizeof(*coeff))
        return AVERROR(EOVERFLOW);
    coeff = av_malloc_array(coefficient_count, sizeof(*coeff));
    rle = av_malloc(ZSXD_MAX_RLE);
    if (!coeff || !rle) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    for (plane = 0; plane < 3; plane++) {
        int width = plane ? s->width / 2 : s->width;
        int height = plane ? s->height / 2 : s->height;
        size_t blocks_w = (width + 7) / 8, block;
        const uint8_t *sp = source + offsets[plane];
        const uint8_t *rp = reference ? reference + offsets[plane] : NULL;
        uint8_t *dp = recon + offsets[plane];
        zsxd_quant_table(s->quality, plane, q[plane]);
        for (block = 0; block < counts[plane]; block++) {
            int bx = block % blocks_w;
            int by = block / blocks_w;
            int16_t *dst = coeff + coeff_offset + block * 64;
            zsxd_forward_block(sp, rp, width, height, bx, by, intra,
                               q[plane], dst);
            zsxd_inverse_block(dst, q[plane], dp, rp, width, height, bx, by,
                               intra);
        }
        planes[plane] = coeff + coeff_offset;
        coeff_offset += counts[plane] * 64;
    }
    if (zsx_coefficient_rle_serialize(planes, counts, 3, rle, ZSXD_MAX_RLE,
                                      raw_size)) {
        ret = AVERROR(EOVERFLOW);
        goto done;
    }
    for (plane = 0; plane < 3; plane++) {
        size_t i, plane_pixels = plane ? (size_t)s->width * s->height / 4 :
                                         (size_t)s->width * s->height;
        const uint8_t *sp = source + offsets[plane];
        const uint8_t *dp = recon + offsets[plane];
        for (i = 0; i < plane_pixels; i++) {
            int d = (int)sp[i] - dp[i];
            error += (uint64_t)(d * d);
        }
    }
    *sse = error;
    *raw = rle;
    rle = NULL;
    ret = 0;
done:
    av_free(coeff);
    av_free(rle);
    return ret;
}

/*
 * The production bridge intentionally accepts only one 12 MiB ZSX chunk.
 * RLE can be larger, therefore the DCT payload carries a count and bounded
 * (raw length, encoded length, production-chunk) records.  It is not an
 * unchecked concatenation and the decoder verifies every record.
 */
static int zsxd_compress_chunks(const uint8_t *raw, size_t raw_size, unsigned profile,
                                uint8_t **payload, size_t *payload_size)
{
    size_t count, capacity, at = 4, offset = 0, i;
    uint8_t *out;
    if (!raw || !raw_size || raw_size > ZSXD_MAX_RLE)
        return AVERROR(EINVAL);
    count = (raw_size + ZSXD_CORE_CHUNK - 1) / ZSXD_CORE_CHUNK;
    if (!count || count > UINT32_MAX ||
        raw_size > SIZE_MAX - 4 - count * 24)
        return AVERROR(EOVERFLOW);
    capacity = raw_size + 4 + count * 24;
    out = av_malloc(capacity);
    if (!out)
        return AVERROR(ENOMEM);
    AV_WL32(out, count);
    for (i = 0; i < count; i++) {
        uint8_t *coded = NULL;
        size_t coded_size = 0;
        size_t wanted = FFMIN((size_t)ZSXD_CORE_CHUNK, raw_size - offset);
        int ret = zsx_ffmpeg_encode_selected(raw + offset, wanted, profile, &coded, &coded_size);
        if (ret < 0 || coded_size < 16 || coded_size > wanted + 16 ||
            coded_size > capacity - at - 8) {
            zsx_ffmpeg_free(coded);
            av_free(out);
            return ret == ZSX_NOMEM ? AVERROR(ENOMEM) : AVERROR_INVALIDDATA;
        }
        AV_WL32(out + at, wanted);
        AV_WL32(out + at + 4, coded_size);
        memcpy(out + at + 8, coded, coded_size);
        at += 8 + coded_size;
        offset += wanted;
        zsx_ffmpeg_free(coded);
    }
    *payload = out;
    *payload_size = at;
    return 0;
}

static int zsxd_decompress_chunks(const uint8_t *payload, size_t payload_size,
                                  size_t raw_size, int version, uint8_t **raw)
{
    const uint8_t *p = payload;
    uint8_t *out;
    size_t count, expected, offset = 0, i;
    if (!payload || payload_size < 4 || !raw_size || raw_size > ZSXD_MAX_RLE)
        return AVERROR_INVALIDDATA;
    count = AV_RL32(p);
    expected = (raw_size + ZSXD_CORE_CHUNK - 1) / ZSXD_CORE_CHUNK;
    if (!count || count != expected)
        return AVERROR_INVALIDDATA;
    out = av_malloc(raw_size);
    if (!out)
        return AVERROR(ENOMEM);
    p += 4;
    for (i = 0; i < count; i++) {
        uint32_t declared, coded_size;
        uint8_t *decoded = NULL;
        size_t decoded_size = 0, wanted = FFMIN((size_t)ZSXD_CORE_CHUNK,
                                                raw_size - offset);
        int ret;
        if ((size_t)(payload + payload_size - p) < 8) {
            av_free(out);
            return AVERROR_INVALIDDATA;
        }
        declared = AV_RL32(p);
        coded_size = AV_RL32(p + 4);
        p += 8;
        if (declared != wanted || coded_size < 16 ||
            coded_size > wanted + 16 ||
            (size_t)(payload + payload_size - p) < coded_size) {
            av_free(out);
            return AVERROR_INVALIDDATA;
        }
        ret = version == ZSX_VIDEO_LEGACY
            ? zsx_ffmpeg_decode(p, coded_size, &decoded, &decoded_size)
            : zsx_ffmpeg_decode_profile(p, coded_size, &decoded, &decoded_size);
        if (ret < 0 || decoded_size != wanted) {
            zsx_ffmpeg_free(decoded);
            av_free(out);
            return ret == ZSX_NOMEM ? AVERROR(ENOMEM) : AVERROR_INVALIDDATA;
        }
        memcpy(out + offset, decoded, wanted);
        zsx_ffmpeg_free(decoded);
        p += coded_size;
        offset += wanted;
    }
    if (p != payload + payload_size || offset != raw_size) {
        av_free(out);
        return AVERROR_INVALIDDATA;
    }
    *raw = out;
    return 0;
}

/*
 * The field at offset 28 is named header CRC for version-1 compatibility,
 * but covers the complete packet (including bridge records and chunks).
 * Its own four bytes are supplied as zero to the CRC calculation.
 */
static uint32_t zsxd_packet_crc(const uint8_t *packet, size_t packet_size)
{
    static const uint8_t zero[4];
    const AVCRC *table = av_crc_get_table(AV_CRC_32_IEEE_LE);
    uint32_t crc;
    if (packet_size < ZSXD_HEADER)
        return 0;
    crc = av_crc(table, 0, packet, 28);
    crc = av_crc(table, crc, zero, sizeof(zero));
    return av_crc(table, crc, packet + ZSXD_HEADER,
                  packet_size - ZSXD_HEADER);
}

static int zsxd_make_packet(ZSXDDCTContext *s, const uint8_t *payload,
                            size_t payload_size, size_t raw_size,
                            uint64_t sse, const uint8_t *recon, int predictive,
                            uint8_t **packet, size_t *packet_size)
{
    uint8_t *out;
    size_t size;
    if (!payload || !payload_size || raw_size > UINT32_MAX ||
        payload_size > UINT32_MAX || payload_size > INT_MAX - ZSXD_HEADER)
        return AVERROR(EOVERFLOW);
    size = ZSXD_HEADER + payload_size;
    out = av_malloc(size);
    if (!out)
        return AVERROR(ENOMEM);
    memcpy(out, "ZSXD", 4);
    out[4] = ZSXD_VERSION;
    out[5] = predictive ? ZSXD_P0 : 0;
    out[6] = s->quality;
    out[7] = 0;
    AV_WL16(out + 8, s->width);
    AV_WL16(out + 10, s->height);
    AV_WL32(out + 12, s->next_index);
    AV_WL32(out + 16, raw_size);
    AV_WL32(out + 20, payload_size);
    AV_WL32(out + 24, zsxd_crc(recon, s->reference_size));
    memcpy(out + ZSXD_HEADER, payload, payload_size);
    AV_WL32(out + 28, zsxd_packet_crc(out, size));
    *packet = out;
    *packet_size = size;
    (void)sse;
    return 0;
}

static int zsxd_init(AVCodecContext *avctx)
{
    ZSXDDCTContext *s = avctx->priv_data;
    size_t counts[3], offsets[3], pixels, coefficients;
    int ret = zsxd_geometry(avctx->width, avctx->height, counts, offsets,
                            &pixels, &coefficients);
    if (ret < 0)
        return ret;
    s->width = avctx->width;
    s->height = avctx->height;
    s->reference_size = pixels;
    if (av_codec_is_encoder(avctx->codec)) {
        if (avctx->pix_fmt != AV_PIX_FMT_YUV420P || s->quality < 1 ||
            s->quality > 100)
            return AVERROR(EINVAL);
        avctx->extradata = av_mallocz(ZSXD_EXTRA + AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata)
            return AVERROR(ENOMEM);
        memcpy(avctx->extradata, "ZSXD", 4);
        avctx->extradata[4] = ZSXD_VERSION;
        avctx->extradata[5] = AV_PIX_FMT_YUV420P;
        avctx->extradata[6] = s->quality;
        avctx->extradata[7] = 0;
        AV_WL32(avctx->extradata + 8, avctx->width);
        AV_WL32(avctx->extradata + 12, avctx->height);
        avctx->extradata_size = ZSXD_EXTRA;
    } else {
        const uint8_t *extra = avctx->extradata;
        if (!extra || avctx->extradata_size != ZSXD_EXTRA ||
            memcmp(extra, "ZSXD", 4) ||
            (extra[4] != ZSX_VIDEO_LEGACY && extra[4] != ZSX_VIDEO_STREAM) ||
            extra[5] != AV_PIX_FMT_YUV420P || !extra[6] ||
            extra[7] || AV_RL32(extra + 8) != (uint32_t)avctx->width ||
            AV_RL32(extra + 12) != (uint32_t)avctx->height)
            return AVERROR_INVALIDDATA;
        s->quality = extra[6];
        s->entropy_version = extra[4];
        avctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }
    return 0;
}

static int zsxd_close(AVCodecContext *avctx)
{
    ZSXDDCTContext *s = avctx->priv_data;
    av_freep(&s->reference);
    s->have_reference = 0;
    return 0;
}

static void zsxd_flush(AVCodecContext *avctx)
{
    ZSXDDCTContext *s = avctx->priv_data;
    av_freep(&s->reference);
    s->have_reference = 0;
    s->next_index = 0;
}

static int zsxd_copy_input(const AVFrame *frame, ZSXDDCTContext *s,
                           uint8_t *destination)
{
    size_t counts[3], offsets[3], pixels, coefficients;
    int plane, ret = zsxd_geometry(s->width, s->height, counts, offsets,
                                   &pixels, &coefficients);
    if (ret < 0 || !frame->data[0] || !frame->data[1] || !frame->data[2])
        return ret < 0 ? ret : AVERROR(EINVAL);
    for (plane = 0; plane < 3; plane++) {
        int y, width = plane ? s->width / 2 : s->width;
        int height = plane ? s->height / 2 : s->height;
        if (frame->linesize[plane] < width)
            return AVERROR(EINVAL);
        for (y = 0; y < height; y++)
            memcpy(destination + offsets[plane] + (size_t)y * width,
                   frame->data[plane] + (ptrdiff_t)y * frame->linesize[plane],
                   width);
    }
    return 0;
}

static int zsxd_archive(ZSXDDCTContext *s, const uint8_t *raw,
                        size_t raw_size, const uint8_t *recon, int predictive,
                        uint8_t **packet, size_t *packet_size)
{
    uint8_t *payload = NULL;
    size_t payload_size = 0;
    int ret = zsxd_compress_chunks(raw, raw_size, s->entropy_profile, &payload, &payload_size);
    if (ret >= 0)
        ret = zsxd_make_packet(s, payload, payload_size, raw_size, 0, recon,
                               predictive, packet, packet_size);
    av_free(payload);
    return ret;
}

static int zsxd_encode(AVCodecContext *avctx, AVPacket *pkt,
                       const AVFrame *frame, int *got_packet)
{
    ZSXDDCTContext *s = avctx->priv_data;
    uint8_t *source = NULL, *recon = NULL, *raw = NULL, *i_packet = NULL;
    uint8_t *p_packet = NULL, *chosen = NULL;
    size_t raw_size = 0, i_size = 0, p_size = 0, chosen_size = 0;
    uint64_t i_sse = 0, p_sse = 0;
    int ret, can_predict, use_p = 0;

    if (!frame || frame->width != s->width || frame->height != s->height ||
        frame->format != AV_PIX_FMT_YUV420P)
        return AVERROR(EINVAL);
    source = av_malloc(s->reference_size);
    recon = av_malloc(s->reference_size);
    if (!source || !recon) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    ret = zsxd_copy_input(frame, s, source);
    if (ret < 0)
        goto done;

    /* An I candidate is always archived first, so comparison is real bytes. */
    ret = zsxd_rle_from_picture(s, source, NULL, 1, recon, &raw, &raw_size,
                                &i_sse);
    if (ret < 0)
        goto done;
    ret = zsxd_archive(s, raw, raw_size, recon, 0, &i_packet, &i_size);
    av_freep(&raw);
    if (ret < 0)
        goto done;
    can_predict = s->have_reference &&
                  (s->next_index % ZSXD_KEY_INTERVAL != 0);
    if (can_predict) {
        /*
         * Retain only the measured I length while testing P0.  If I wins it
         * is regenerated; this keeps the largest legal frame below the
         * intended bounded-work-memory envelope.
         */
        av_freep(&i_packet);
        ret = zsxd_rle_from_picture(s, source, s->reference, 0, recon, &raw,
                                    &raw_size, &p_sse);
        if (ret < 0)
            goto done;
        /* 1.05 is expressed as exact integer arithmetic. */
        if (p_sse <= UINT64_MAX / 100 &&
            p_sse * 100 <= i_sse * 105) {
            ret = zsxd_archive(s, raw, raw_size, recon, 1, &p_packet, &p_size);
            if (ret < 0)
                goto done;
            if (p_size < i_size) {
                use_p = 1;
                chosen = p_packet;
                chosen_size = p_size;
                p_packet = NULL;
            }
        }
        av_freep(&raw);
    }
    if (!use_p) {
        /*
         * Reconstruct and rearchive I only when its initial packet was freed
         * for P0 selection.  No P0 production chunk is emitted when P0 is
         * ineligible by the measured SSE gate.
         */
        if (!i_packet) {
            ret = zsxd_rle_from_picture(s, source, NULL, 1, recon, &raw,
                                        &raw_size, &i_sse);
            if (ret < 0)
                goto done;
            ret = zsxd_archive(s, raw, raw_size, recon, 0, &i_packet, &i_size);
            av_freep(&raw);
            if (ret < 0)
                goto done;
        } else {
            /* recon already contains the I reconstruction if P0 was skipped. */
        }
        chosen = i_packet;
        chosen_size = i_size;
        i_packet = NULL;
    }
    ret = ff_get_encode_buffer(avctx, pkt, chosen_size, 0);
    if (ret < 0)
        goto done;
    memcpy(pkt->data, chosen, chosen_size);
    if (!use_p)
        pkt->flags |= AV_PKT_FLAG_KEY;
    av_freep(&s->reference);
    s->reference = av_malloc(s->reference_size);
    if (!s->reference) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    memcpy(s->reference, recon, s->reference_size);
    s->have_reference = 1;
    s->next_index++;
    *got_packet = 1;
    ret = 0;
done:
    av_free(source);
    av_free(recon);
    av_free(raw);
    av_free(i_packet);
    av_free(p_packet);
    av_free(chosen);
    return ret;
}

static int zsxd_reconstruct(ZSXDDCTContext *s, const int16_t *coeff,
                            const uint8_t *reference, int intra,
                            uint8_t *recon)
{
    size_t counts[3], offsets[3], pixels, coefficient_count, coeff_offset = 0;
    uint16_t q[64];
    int plane, ret = zsxd_geometry(s->width, s->height, counts, offsets,
                                   &pixels, &coefficient_count);
    if (ret < 0 || pixels != s->reference_size)
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    for (plane = 0; plane < 3; plane++) {
        int width = plane ? s->width / 2 : s->width;
        int height = plane ? s->height / 2 : s->height;
        size_t blocks_w = (width + 7) / 8, block;
        uint8_t *dp = recon + offsets[plane];
        const uint8_t *rp = reference ? reference + offsets[plane] : NULL;
        zsxd_quant_table(s->quality, plane, q);
        for (block = 0; block < counts[plane]; block++)
            zsxd_inverse_block(coeff + coeff_offset + block * 64, q, dp, rp,
                               width, height, block % blocks_w,
                               block / blocks_w, intra);
        coeff_offset += counts[plane] * 64;
    }
    return 0;
}

static int zsxd_copy_output(AVFrame *frame, ZSXDDCTContext *s,
                            const uint8_t *source)
{
    size_t counts[3], offsets[3], pixels, coefficients;
    int plane, ret = zsxd_geometry(s->width, s->height, counts, offsets,
                                   &pixels, &coefficients);
    if (ret < 0)
        return ret;
    for (plane = 0; plane < 3; plane++) {
        int y, width = plane ? s->width / 2 : s->width;
        int height = plane ? s->height / 2 : s->height;
        for (y = 0; y < height; y++)
            memcpy(frame->data[plane] + (ptrdiff_t)y * frame->linesize[plane],
                   source + offsets[plane] + (size_t)y * width, width);
    }
    return 0;
}

static int zsxd_decode(AVCodecContext *avctx, AVFrame *frame,
                       int *got_frame, AVPacket *avpkt)
{
    ZSXDDCTContext *s = avctx->priv_data;
    const uint8_t *p = avpkt->data;
    uint32_t index, raw_size, payload_size, stored_recon_crc, stored_header_crc;
    int predictive, ret = 0;
    size_t counts[3], offsets[3], pixels, coefficient_count, samples = 0;
    uint8_t *raw = NULL, *recon = NULL;
    int16_t *coeff = NULL;

    if (avpkt->size < ZSXD_HEADER || !p || memcmp(p, "ZSXD", 4) ||
        p[4] != s->entropy_version || (p[5] & ~ZSXD_P0) || p[6] != s->quality ||
        p[7] || AV_RL16(p + 8) != s->width || AV_RL16(p + 10) != s->height)
        return AVERROR_INVALIDDATA;
    predictive = !!(p[5] & ZSXD_P0);
    index = AV_RL32(p + 12);
    raw_size = AV_RL32(p + 16);
    payload_size = AV_RL32(p + 20);
    stored_recon_crc = AV_RL32(p + 24);
    stored_header_crc = AV_RL32(p + 28);
    if (!raw_size || raw_size > ZSXD_MAX_RLE ||
        payload_size < 4 || payload_size > INT_MAX - ZSXD_HEADER ||
        (size_t)payload_size + ZSXD_HEADER != (size_t)avpkt->size ||
        zsxd_packet_crc(p, avpkt->size) != stored_header_crc)
        return AVERROR_INVALIDDATA;
    if (predictive) {
        if (!s->have_reference || index != s->next_index)
            return AVERROR_INVALIDDATA;
    } else if (s->have_reference && index != s->next_index) {
        /*
         * An I picture may start a fresh seek/decode run at any frame index;
         * non-discontinuous I pictures still keep the index chain strict.
         */
        return AVERROR_INVALIDDATA;
    }
    ret = zsxd_geometry(s->width, s->height, counts, offsets, &pixels,
                        &coefficient_count);
    if (ret < 0 || pixels != s->reference_size ||
        coefficient_count > SIZE_MAX / sizeof(*coeff))
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    ret = zsxd_decompress_chunks(p + ZSXD_HEADER, payload_size, raw_size,
                                 s->entropy_version, &raw);
    if (ret < 0)
        goto done;
    coeff = av_malloc_array(coefficient_count, sizeof(*coeff));
    recon = av_malloc(s->reference_size);
    if (!coeff || !recon) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    if (zsx_coefficient_rle_parse(raw, raw_size, counts, 3, coeff,
                                  coefficient_count, &samples) ||
        samples != coefficient_count) {
        ret = AVERROR_INVALIDDATA;
        goto done;
    }
    ret = zsxd_reconstruct(s, coeff, predictive ? s->reference : NULL,
                           !predictive, recon);
    if (ret < 0 || zsxd_crc(recon, s->reference_size) != stored_recon_crc) {
        ret = AVERROR_INVALIDDATA;
        goto done;
    }
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        goto done;
    ret = zsxd_copy_output(frame, s, recon);
    if (ret < 0)
        goto done;
    av_freep(&s->reference);
    s->reference = recon;
    recon = NULL;
    s->have_reference = 1;
    s->next_index = index + 1;
    frame->pict_type = predictive ? AV_PICTURE_TYPE_P : AV_PICTURE_TYPE_I;
    if (!predictive)
        frame->flags |= AV_FRAME_FLAG_KEY;
    *got_frame = 1;
    ret = avpkt->size;
done:
    av_free(raw);
    av_free(coeff);
    av_free(recon);
    return ret;
}

const FFCodec ff_zsx_dct_video_encoder = {
    .p.name         = "zsx_dct_video",
    CODEC_LONG_NAME("ZSX deterministic DCT video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ZSX_DCT_VIDEO,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(ZSXDDCTContext),
    .p.priv_class   = &zsxd_class,
    .init           = zsxd_init,
    .close          = zsxd_close,
    .flush          = zsxd_flush,
    FF_CODEC_ENCODE_CB(zsxd_encode),
    .p.pix_fmts     = zsxd_formats,
};

const FFCodec ff_zsx_dct_video_decoder = {
    .p.name         = "zsx_dct_video",
    CODEC_LONG_NAME("ZSX deterministic DCT video"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ZSX_DCT_VIDEO,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .priv_data_size = sizeof(ZSXDDCTContext),
    .init           = zsxd_init,
    .close          = zsxd_close,
    .flush          = zsxd_flush,
    FF_CODEC_DECODE_CB(zsxd_decode),
    .p.pix_fmts     = zsxd_formats,
};