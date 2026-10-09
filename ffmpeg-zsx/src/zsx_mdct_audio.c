/*
 * ZSX MDCT audio: independently decodable, AAC-sized long blocks.
 *
 * This is deliberately not AAC and does not share the AMZSX Python container.
 * Packets contain a complete 1..16384-sample PCM chunk, including the final
 * MDCT block needed to close TDAC.  The coefficient byte stream is compressed
 * with the production ZSX core, but there is no prediction or overlap state
 * between packets.
 */
#include "config.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "internal.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "libavutil/channel_layout.h"
#include "libavutil/common.h"
#include "libavutil/crc.h"
#include "libavutil/error.h"
#include "libavutil/intfloat.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/mem.h"
#include "libavutil/opt.h"
#include "libavutil/samplefmt.h"

#include "zsx_core_api.h"

#define ZSXM_VERSION       2
#define ZSXM_BINS          1024
#define ZSXM_WINDOW        2048
#define ZSXM_FFT           4096
#define ZSXM_FRAME_SIZE    16384
#define ZSXM_MAX_CHANNELS  2
#define ZSXM_MAX_RAW       (12 * 1024 * 1024)
#define ZSXM_HEADER        44
#define ZSXM_FLAG_MS       1
#define ZSXM_PI            3.14159265358979323846264338327950288
#define ZSXM_SQRT2         1.41421356237309504880168872420969808

typedef struct ZSXMComplex {
    double re, im;
} ZSXMComplex;

typedef struct ZSXMDCTContext {
    const AVClass *class;
    double step;
    int ms;
} ZSXMDCTContext;

#define OFFSET(x) offsetof(ZSXMDCTContext, x)
#define AE AV_OPT_FLAG_AUDIO_PARAM | AV_OPT_FLAG_ENCODING_PARAM
static const AVOption zsx_mdct_audio_options[] = {
    { "zsx_step", "quantisation step (quality control, not a bitrate target)",
      OFFSET(step), AV_OPT_TYPE_DOUBLE, { .dbl = 0.016 }, 1e-9, 1000.0, AE },
    { "zsx_ms", "use orthonormal mid/side coding for stereo",
      OFFSET(ms), AV_OPT_TYPE_BOOL, { .i64 = 1 }, 0, 1, AE },
    { NULL },
};

static const AVClass zsx_mdct_audio_class = {
    .class_name = "zsx_mdct_audio",
    .item_name  = av_default_item_name,
    .option     = zsx_mdct_audio_options,
    .version    = LIBAVUTIL_VERSION_INT,
};

static const enum AVSampleFormat zsx_mdct_audio_formats[] = {
    AV_SAMPLE_FMT_FLT, AV_SAMPLE_FMT_NONE,
};

static uint32_t zsxm_crc(const uint8_t *data, size_t size)
{
    return av_crc(av_crc_get_table(AV_CRC_32_IEEE_LE), 0, data, size);
}

/* Iterative radix-2 DFT.  This avoids a per-frame cosine matrix (O(N log N)). */
static void zsxm_fft(ZSXMComplex *a, int inverse)
{
    int i, j, len;
    for (i = 1, j = 0; i < ZSXM_FFT; i++) {
        int bit = ZSXM_FFT >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) {
            ZSXMComplex t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    for (len = 2; len <= ZSXM_FFT; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * ZSXM_PI / len;
        const double wr_step = cos(angle), wi_step = sin(angle);
        double wr = 1.0, wi = 0.0;
        int half = len >> 1;
        for (j = 0; j < half; j++) {
            for (i = j; i < ZSXM_FFT; i += len) {
                ZSXMComplex u = a[i], v = a[i + half];
                double vr = v.re * wr - v.im * wi;
                double vi = v.re * wi + v.im * wr;
                a[i].re = u.re + vr;
                a[i].im = u.im + vi;
                a[i + half].re = u.re - vr;
                a[i + half].im = u.im - vi;
            }
            {
                double next = wr * wr_step - wi * wi_step;
                wi = wr * wi_step + wi * wr_step;
                wr = next;
            }
        }
    }
    if (inverse)
        for (i = 0; i < ZSXM_FFT; i++) {
            a[i].re /= ZSXM_FFT;
            a[i].im /= ZSXM_FFT;
        }
}

static double zsxm_window(int n)
{
    return sin(ZSXM_PI * (n + 0.5) / (2.0 * ZSXM_BINS));
}

static void zsxm_analysis(const float *pcm, int samples, int channels,
                          int use_ms, int frame, int channel,
                          double *coeff, ZSXMComplex *fft)
{
    int i;
    const int start = frame * ZSXM_BINS - ZSXM_BINS;
    const double inv_sqrt = 1.0 / sqrt((double)ZSXM_BINS);
    memset(fft, 0, sizeof(*fft) * ZSXM_FFT);
    for (i = 0; i < ZSXM_WINDOW; i++) {
        int source = start + i;
        double value = 0.0;
        if (source >= 0 && source < samples) {
            if (use_ms) {
                double l = pcm[(size_t)source * channels];
                double r = pcm[(size_t)source * channels + 1];
                value = (channel ? l - r : l + r) / ZSXM_SQRT2;
            } else {
                value = pcm[(size_t)source * channels + channel];
            }
        }
        fft[i].re = value * zsxm_window(i);
    }
    zsxm_fft(fft, 0);
    for (i = 0; i < ZSXM_BINS; i++) {
        int k = 2 * i + 1;
        double phase = ZSXM_PI / ZSXM_BINS *
                       (0.5 + ZSXM_BINS / 2.0) * (i + 0.5);
        /* Re(conj(X) * exp(i phase)), exactly the reference FFT formulation. */
        coeff[i] = (fft[k].re * cos(phase) + fft[k].im * sin(phase)) * inv_sqrt;
    }
}

static void zsxm_synthesis(const int32_t *quant, double step, int frame,
                           int channel, int channels, double *out,
                           ZSXMComplex *fft)
{
    int i;
    const double scale = 4.0 * sqrt((double)ZSXM_BINS);
    memset(fft, 0, sizeof(*fft) * ZSXM_FFT);
    for (i = 0; i < ZSXM_BINS; i++) {
        int k = 2 * i + 1;
        int32_t q = quant[((size_t)frame * channels + channel) * ZSXM_BINS + i];
        double value = copysign(pow(fabs((double)q), 4.0 / 3.0), (double)q) * step;
        double phase = ZSXM_PI / ZSXM_BINS *
                       (0.5 + ZSXM_BINS / 2.0) * (i + 0.5);
        fft[k].re = scale * value * cos(phase);
        fft[k].im = scale * value * sin(phase);
        fft[ZSXM_FFT - k].re = fft[k].re;
        fft[ZSXM_FFT - k].im = -fft[k].im;
    }
    zsxm_fft(fft, 1);
    for (i = 0; i < ZSXM_WINDOW; i++)
        out[(size_t)(frame * ZSXM_BINS + i) * channels + channel] +=
            fft[i].re * zsxm_window(i);
}

static int zsxm_put_varint(uint8_t **dst, const uint8_t *end, uint32_t value)
{
    do {
        if (*dst >= end)
            return AVERROR(EOVERFLOW);
        *(*dst)++ = (value & ~0x7fU ? 0x80 : 0) | (value & 0x7f);
        value >>= 7;
    } while (value);
    return 0;
}

static int zsxm_get_varint(const uint8_t **src, const uint8_t *end, uint32_t *value)
{
    uint32_t result = 0;
    int i;
    for (i = 0; i < 5; i++) {
        uint8_t byte;
        if (*src >= end)
            return AVERROR_INVALIDDATA;
        byte = *(*src)++;
        if (i == 4 && byte > 0x0f)
            return AVERROR_INVALIDDATA;
        result |= (uint32_t)(byte & 0x7f) << (7 * i);
        if (!(byte & 0x80)) {
            *value = result;
            return 0;
        }
    }
    return AVERROR_INVALIDDATA;
}

static int zsxm_init(AVCodecContext *avctx)
{
    static const uint8_t magic[] = { 'Z', 'S', 'X', 'M', ZSXM_VERSION,
                                     AV_SAMPLE_FMT_FLT };
    uint64_t mask = 0;

    if (av_codec_is_encoder(avctx->codec)) {
        ZSXMDCTContext *s = avctx->priv_data;
        if (!isfinite(s->step) || s->step <= 0 || s->step > FLT_MAX ||
            avctx->ch_layout.nb_channels < 1 ||
            avctx->ch_layout.nb_channels > ZSXM_MAX_CHANNELS ||
            (avctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC &&
             avctx->ch_layout.order != AV_CHANNEL_ORDER_NATIVE))
            return AVERROR(EINVAL);
        if (avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE)
            mask = avctx->ch_layout.u.mask;
        avctx->extradata = av_mallocz(sizeof(magic) + 10 +
                                      AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata)
            return AVERROR(ENOMEM);
        memcpy(avctx->extradata, magic, sizeof(magic));
        avctx->extradata[6] = avctx->ch_layout.order;
        avctx->extradata[7] = avctx->ch_layout.nb_channels;
        AV_WL64(avctx->extradata + 8, mask);
        avctx->extradata_size = sizeof(magic) + 10;
        avctx->frame_size = ZSXM_FRAME_SIZE;
        return 0;
    }
    if (avctx->extradata_size != (int)sizeof(magic) + 10 ||
        memcmp(avctx->extradata, magic, sizeof(magic)) ||
        (avctx->extradata[6] != AV_CHANNEL_ORDER_UNSPEC &&
         avctx->extradata[6] != AV_CHANNEL_ORDER_NATIVE) ||
        !avctx->extradata[7] || avctx->extradata[7] > ZSXM_MAX_CHANNELS)
        return AVERROR_INVALIDDATA;
    avctx->sample_fmt = AV_SAMPLE_FMT_FLT;
    av_channel_layout_uninit(&avctx->ch_layout);
    avctx->ch_layout.order = avctx->extradata[6];
    avctx->ch_layout.nb_channels = avctx->extradata[7];
    if (avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE)
        avctx->ch_layout.u.mask = AV_RL64(avctx->extradata + 8);
    return 0;
}

static int zsxm_encode(AVCodecContext *avctx, AVPacket *pkt,
                       const AVFrame *frame, int *got_packet)
{
    ZSXMDCTContext *s = avctx->priv_data;
    const int channels = avctx->ch_layout.nb_channels;
    int samples, frames, ret = 0, i, f, ch;
    size_t coeff_count, raw_capacity, raw_size, coded_size = 0, packet_size;
    uint8_t *raw = NULL, *coded = NULL, *dst;
    double *coeff = NULL;
    ZSXMComplex *fft = NULL;
    float wire_step;

    if (!frame || frame->format != AV_SAMPLE_FMT_FLT || !frame->extended_data ||
        !frame->extended_data[0] || frame->ch_layout.nb_channels != channels ||
        frame->nb_samples < 1 || frame->nb_samples > ZSXM_FRAME_SIZE)
        return AVERROR(EINVAL);
    samples = frame->nb_samples;
    frames = (samples + 2 * ZSXM_BINS - 1) / ZSXM_BINS;
    if (av_size_mult((size_t)frames * channels, ZSXM_BINS, &coeff_count) ||
        av_size_mult(coeff_count, 5, &raw_capacity) || raw_capacity > ZSXM_MAX_RAW)
        return AVERROR(EOVERFLOW);
    wire_step = s->step;
    if (!isfinite(wire_step) || wire_step <= 0)
        return AVERROR(EINVAL);
    coeff = av_malloc_array(ZSXM_BINS, sizeof(*coeff));
    fft = av_malloc_array(ZSXM_FFT, sizeof(*fft));
    raw = av_malloc(raw_capacity);
    if (!coeff || !fft || !raw) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    for (i = 0; i < samples * channels; i++)
        if (!isfinite(((const float *)frame->extended_data[0])[i])) {
            ret = AVERROR_INVALIDDATA;
            goto done;
        }
    dst = raw;
    for (f = 0; f < frames; f++) for (ch = 0; ch < channels; ch++) {
        zsxm_analysis((const float *)frame->extended_data[0], samples, channels,
                      s->ms && channels == 2, f, ch, coeff, fft);
        for (i = 0; i < ZSXM_BINS; i++) {
            double qd = copysign(floor(pow(fabs(coeff[i] / wire_step), .75) + .5),
                                 coeff[i]);
            int32_t q;
            if (!isfinite(qd) || qd > INT32_MAX || qd < INT32_MIN) {
                ret = AVERROR(EOVERFLOW);
                goto done;
            }
            q = (int32_t)qd;
            ret = zsxm_put_varint(&dst, raw + raw_capacity,
                                  ((uint32_t)q << 1) ^ -(uint32_t)(q < 0));
            if (ret < 0)
                goto done;
        }
    }
    raw_size = dst - raw;
    ret = zsx_ffmpeg_encode(raw, raw_size, &coded, &coded_size);
    if (ret < 0 || coded_size > ZSXM_MAX_RAW || coded_size > INT_MAX) {
        ret = ret < 0 ? AVERROR_INVALIDDATA : AVERROR(EOVERFLOW);
        goto done;
    }
    if (coded_size > INT_MAX - ZSXM_HEADER) {
        ret = AVERROR(EOVERFLOW);
        goto done;
    }
    packet_size = ZSXM_HEADER + coded_size;
    ret = ff_get_encode_buffer(avctx, pkt, packet_size, 0);
    if (ret < 0)
        goto done;
    memcpy(pkt->data, "ZSXM", 4);
    pkt->data[4] = ZSXM_VERSION;
    pkt->data[5] = s->ms && channels == 2 ? ZSXM_FLAG_MS : 0;
    pkt->data[6] = channels;
    pkt->data[7] = avctx->ch_layout.order;
    AV_WL32(pkt->data + 8, samples);
    AV_WL32(pkt->data + 12, frames);
    AV_WL16(pkt->data + 16, ZSXM_BINS);
    AV_WL16(pkt->data + 18, 0);
    AV_WL32(pkt->data + 20, av_float2int(wire_step));
    AV_WL64(pkt->data + 24, avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ?
            avctx->ch_layout.u.mask : 0);
    AV_WL32(pkt->data + 32, raw_size);
    AV_WL32(pkt->data + 36, coded_size);
    AV_WL32(pkt->data + 40, 0);
    memcpy(pkt->data + ZSXM_HEADER, coded, coded_size);
    AV_WL32(pkt->data + 40, zsxm_crc(pkt->data, packet_size));
    *got_packet = 1;

done:
    zsx_ffmpeg_free(coded);
    av_free(raw);
    av_free(coeff);
    av_free(fft);
    return ret;
}

static int zsxm_decode(AVCodecContext *avctx, AVFrame *frame,
                       int *got_frame, AVPacket *avpkt)
{
    const uint8_t *p = avpkt->data, *src, *end;
    const int channels = avctx->ch_layout.nb_channels;
    uint32_t samples, frames, raw_size, coded_size, stored_crc, value;
    uint64_t mask;
    float wire_step;
    uint8_t *check = NULL, *raw = NULL;
    size_t coeff_count, output_count, decoded_size = 0, packet_size;
    int32_t *quant = NULL;
    double *overlap = NULL;
    ZSXMComplex *fft = NULL;
    int ret = 0, f, ch, i, use_ms;

    if (avpkt->size < ZSXM_HEADER || memcmp(p, "ZSXM", 4) ||
        p[4] != ZSXM_VERSION || (p[5] & ~ZSXM_FLAG_MS) ||
        p[6] != channels || p[7] != avctx->ch_layout.order ||
        AV_RL16(p + 16) != ZSXM_BINS || AV_RL16(p + 18))
        return AVERROR_INVALIDDATA;
    samples = AV_RL32(p + 8);
    frames = AV_RL32(p + 12);
    raw_size = AV_RL32(p + 32);
    coded_size = AV_RL32(p + 36);
    stored_crc = AV_RL32(p + 40);
    wire_step = av_int2float(AV_RL32(p + 20));
    mask = AV_RL64(p + 24);
    use_ms = !!(p[5] & ZSXM_FLAG_MS);
    if (!samples || samples > ZSXM_FRAME_SIZE || !frames ||
        frames != (samples + 2 * ZSXM_BINS - 1) / ZSXM_BINS ||
        raw_size > ZSXM_MAX_RAW || coded_size > ZSXM_MAX_RAW ||
        !isfinite(wire_step) || wire_step <= 0 ||
        (use_ms && channels != 2) ||
        (avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE &&
         mask != avctx->ch_layout.u.mask) ||
        (avctx->ch_layout.order != AV_CHANNEL_ORDER_NATIVE && mask) ||
        coded_size > INT_MAX - ZSXM_HEADER)
        return AVERROR_INVALIDDATA;
    packet_size = ZSXM_HEADER + coded_size;
    if (packet_size != (size_t)avpkt->size)
        return AVERROR_INVALIDDATA;
    if (av_size_mult((size_t)frames * channels, ZSXM_BINS, &coeff_count) ||
        raw_size < coeff_count || raw_size > coeff_count * 5 ||
        av_size_mult((size_t)(frames * ZSXM_BINS + ZSXM_BINS), channels,
                     &output_count))
        return AVERROR_INVALIDDATA;
    check = av_memdup(p, avpkt->size);
    if (!check)
        return AVERROR(ENOMEM);
    AV_WL32(check + 40, 0);
    if (zsxm_crc(check, avpkt->size) != stored_crc) {
        ret = AVERROR_INVALIDDATA;
        goto done;
    }
    ret = zsx_ffmpeg_decode(p + ZSXM_HEADER, coded_size, &raw, &decoded_size);
    if (ret < 0 || decoded_size != raw_size) {
        ret = AVERROR_INVALIDDATA;
        goto done;
    }
    quant = av_malloc_array(coeff_count, sizeof(*quant));
    overlap = av_calloc(output_count, sizeof(*overlap));
    fft = av_malloc_array(ZSXM_FFT, sizeof(*fft));
    if (!quant || !overlap || !fft) {
        ret = AVERROR(ENOMEM);
        goto done;
    }
    src = raw;
    end = raw + raw_size;
    for (i = 0; i < coeff_count; i++) {
        ret = zsxm_get_varint(&src, end, &value);
        if (ret < 0)
            goto done;
        quant[i] = (int32_t)((value >> 1) ^ -(int32_t)(value & 1));
    }
    if (src != end) {
        ret = AVERROR_INVALIDDATA;
        goto done;
    }
    for (f = 0; f < (int)frames; f++)
        for (ch = 0; ch < channels; ch++)
            zsxm_synthesis(quant, wire_step, f, ch, channels, overlap, fft);
    frame->nb_samples = samples;
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        goto done;
    for (i = 0; i < (int)samples; i++) {
        for (ch = 0; ch < channels; ch++) {
            double v;
            if (use_ms) {
                double mid = overlap[(size_t)(i + ZSXM_BINS) * channels];
                double side = overlap[(size_t)(i + ZSXM_BINS) * channels + 1];
                v = (ch ? mid - side : mid + side) / ZSXM_SQRT2;
            } else {
                v = overlap[(size_t)(i + ZSXM_BINS) * channels + ch];
            }
            if (!isfinite(v) || fabs(v) > FLT_MAX) {
                ret = AVERROR_INVALIDDATA;
                goto done;
            }
            ((float *)frame->extended_data[0])[(size_t)i * channels + ch] = v;
        }
    }
    frame->sample_rate = avctx->sample_rate;
    *got_frame = 1;
    ret = avpkt->size;
done:
    av_free(check);
    zsx_ffmpeg_free(raw);
    av_free(quant);
    av_free(overlap);
    av_free(fft);
    return ret;
}

const FFCodec ff_zsx_mdct_audio_encoder = {
    .p.name         = "zsx_mdct_audio",
    CODEC_LONG_NAME("ZSX experimental MDCT audio"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ZSX_MDCT_AUDIO,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_SMALL_LAST_FRAME |
                      AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .priv_data_size = sizeof(ZSXMDCTContext),
    .p.priv_class   = &zsx_mdct_audio_class,
    .init           = zsxm_init,
    FF_CODEC_ENCODE_CB(zsxm_encode),
    .p.sample_fmts  = zsx_mdct_audio_formats,
};

const FFCodec ff_zsx_mdct_audio_decoder = {
    .p.name         = "zsx_mdct_audio",
    CODEC_LONG_NAME("ZSX experimental MDCT audio"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ZSX_MDCT_AUDIO,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .init           = zsxm_init,
    FF_CODEC_DECODE_CB(zsxm_decode),
    .p.sample_fmts  = zsx_mdct_audio_formats,
};