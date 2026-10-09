/*
 * Native FFmpeg ZSX audio codec.
 *
 * The supported representation is interleaved little-endian float32
 * (AV_SAMPLE_FMT_FLT).  One AVFrame becomes one canonical ZSX chunk; NUT
 * carries the sample rate, channel layout, timestamps, and frame duration.
 */
#include "config.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "internal.h"
#include <limits.h>
#include "libavutil/error.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"

#include "zsx_core_api.h"

static const enum AVSampleFormat zsx_audio_formats[] = {
    AV_SAMPLE_FMT_FLT,
    AV_SAMPLE_FMT_NONE,
};

static int zsx_audio_init(AVCodecContext *avctx)
{
    static const uint8_t magic[] = { 'Z', 'S', 'X', 'A', 1, AV_SAMPLE_FMT_FLT };
    uint64_t mask;

    if (av_codec_is_encoder(avctx->codec)) {
        if (avctx->ch_layout.nb_channels <= 0 ||
            avctx->ch_layout.nb_channels > UINT8_MAX ||
            (avctx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC &&
             avctx->ch_layout.order != AV_CHANNEL_ORDER_NATIVE))
            return AVERROR(EINVAL);
        mask = avctx->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ?
               avctx->ch_layout.u.mask : 0;
        avctx->extradata = av_mallocz(sizeof(magic) + 1 + 1 + 8 +
                                      AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata)
            return AVERROR(ENOMEM);
        memcpy(avctx->extradata, magic, sizeof(magic));
        avctx->extradata[sizeof(magic)] = avctx->ch_layout.order;
        avctx->extradata[sizeof(magic) + 1] =
            avctx->ch_layout.nb_channels;
        for (int i = 0; i < 8; i++)
            avctx->extradata[sizeof(magic) + 2 + i] = mask >> (8 * i);
        avctx->extradata_size = sizeof(magic) + 1 + 1 + 8;
        return 0;
    }
    if (avctx->extradata_size != sizeof(magic) + 1 + 1 + 8 ||
        memcmp(avctx->extradata, magic, sizeof(magic)))
        return AVERROR_INVALIDDATA;
    avctx->sample_fmt = AV_SAMPLE_FMT_FLT;
    {
        AVChannelLayout layout = { 0 };
        uint64_t serialized_mask = 0;
        int order = avctx->extradata[sizeof(magic)];
        int channels = avctx->extradata[sizeof(magic) + 1];
        for (int i = 0; i < 8; i++)
            serialized_mask |= (uint64_t)avctx->extradata[sizeof(magic) + 2 + i]
                               << (8 * i);
        if (channels <= 0 ||
            (order != AV_CHANNEL_ORDER_UNSPEC &&
             order != AV_CHANNEL_ORDER_NATIVE))
            return AVERROR_INVALIDDATA;
        layout.order = order;
        layout.nb_channels = channels;
        if (order == AV_CHANNEL_ORDER_NATIVE)
            layout.u.mask = serialized_mask;
        av_channel_layout_uninit(&avctx->ch_layout);
        avctx->ch_layout = layout;
    }
    return 0;
}

#if HAVE_BIGENDIAN
static void zsx_swap_f32_bytes(uint8_t *data, size_t size)
{
    size_t i;
    for (i = 0; i < size; i += 4) {
        uint8_t t = data[i];
        data[i] = data[i + 3];
        data[i + 3] = t;
        t = data[i + 1];
        data[i + 1] = data[i + 2];
        data[i + 2] = t;
    }
}
#endif

static int zsx_audio_encode(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    uint8_t *coded = NULL;
    uint8_t *raw = NULL;
    size_t coded_size = 0;
    int channels = avctx->ch_layout.nb_channels;
    int raw_size;
    int ret;

    if (frame->format != AV_SAMPLE_FMT_FLT || channels <= 0 ||
        frame->nb_samples <= 0 ||
        frame->ch_layout.nb_channels != channels ||
        !frame->extended_data || !frame->extended_data[0])
        return AVERROR(EINVAL);

    raw_size = av_samples_get_buffer_size(NULL, channels, frame->nb_samples,
                                           AV_SAMPLE_FMT_FLT, 1);
    if (raw_size <= 0)
        return AVERROR(EINVAL);
    raw = av_malloc((size_t)raw_size);
    if (!raw)
        return AVERROR(ENOMEM);
    memcpy(raw, frame->extended_data[0], (size_t)raw_size);
#if HAVE_BIGENDIAN
    /* AV_SAMPLE_FMT_FLT is native-endian; the ZSX audio schema is LE. */
    zsx_swap_f32_bytes(raw, (size_t)raw_size);
#endif

    ret = zsx_ffmpeg_encode(raw, (size_t)raw_size, &coded, &coded_size);
    if (ret < 0 || coded_size > INT_MAX) {
        ret = ret < 0 ? AVERROR_INVALIDDATA : AVERROR(EOVERFLOW);
        goto done;
    }
    ret = ff_get_encode_buffer(avctx, pkt, (int)coded_size, 0);
    if (ret < 0)
        goto done;
    memcpy(pkt->data, coded, coded_size);
    *got_packet = 1;
    ret = 0;

done:
    zsx_ffmpeg_free(coded);
    av_free(raw);
    return ret;
}

static int zsx_audio_decode(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame, AVPacket *avpkt)
{
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    int channels = avctx->ch_layout.nb_channels;
    int sample_bytes;
    int nb_samples;
    int expected;
    int ret;

    if (avctx->sample_fmt != AV_SAMPLE_FMT_FLT || channels <= 0)
        return AVERROR_INVALIDDATA;
    sample_bytes = av_get_bytes_per_sample(AV_SAMPLE_FMT_FLT);
    if (sample_bytes <= 0 ||
        avpkt->size < channels * sample_bytes)
        return AVERROR_INVALIDDATA;

    ret = zsx_ffmpeg_decode(avpkt->data, (size_t)avpkt->size,
                            &decoded, &decoded_size);
    if (ret < 0 || decoded_size % ((size_t)channels * sample_bytes)) {
        zsx_ffmpeg_free(decoded);
        return AVERROR_INVALIDDATA;
    }
    nb_samples = (int)(decoded_size / ((size_t)channels * sample_bytes));
    if (nb_samples <= 0)
        goto invalid;
    expected = av_samples_get_buffer_size(NULL, channels, nb_samples,
                                          AV_SAMPLE_FMT_FLT, 1);
    if (expected < 0 || (size_t)expected != decoded_size)
        goto invalid;

    frame->nb_samples = nb_samples;
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        goto done;
#if HAVE_BIGENDIAN
    /* Convert the serialized little-endian samples to native float32. */
    zsx_swap_f32_bytes(decoded, decoded_size);
#endif
    memcpy(frame->extended_data[0], decoded, decoded_size);
    frame->sample_rate = avctx->sample_rate;
    *got_frame = 1;
    ret = avpkt->size;
    goto done;

invalid:
    ret = AVERROR_INVALIDDATA;
done:
    zsx_ffmpeg_free(decoded);
    return ret;
}

const FFCodec ff_zsx_audio_encoder = {
    .p.name         = "zsx_audio",
    CODEC_LONG_NAME("ZSX production audio (interleaved float32 chunks)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ZSX_AUDIO,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_VARIABLE_FRAME_SIZE |
                      AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .init           = zsx_audio_init,
    FF_CODEC_ENCODE_CB(zsx_audio_encode),
    .p.sample_fmts  = zsx_audio_formats,
};

const FFCodec ff_zsx_audio_decoder = {
    .p.name         = "zsx_audio",
    CODEC_LONG_NAME("ZSX production audio (interleaved float32 chunks)"),
    .p.type         = AVMEDIA_TYPE_AUDIO,
    .p.id           = AV_CODEC_ID_ZSX_AUDIO,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .init           = zsx_audio_init,
    FF_CODEC_DECODE_CB(zsx_audio_decode),
    .p.sample_fmts  = zsx_audio_formats,
};