/*
 * Native FFmpeg ZSX video codec.
 *
 * Each packet is exactly one canonical production-ZSX chunk.  The frame is
 * first made packed with av_image_copy_to_buffer(), so input AVFrame strides
 * never become part of the payload.  The decoder uses av_image_copy() to
 * restore FFmpeg's normal per-plane strides.
 */
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"
#include "encode.h"
#include "internal.h"
#include <limits.h>
#include "libavutil/error.h"
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "libavutil/pixdesc.h"

#include "zsx_core_api.h"

static const enum AVPixelFormat zsx_video_formats[] = {
    AV_PIX_FMT_YUV420P,
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_GRAY8,
    AV_PIX_FMT_NONE,
};

static int zsx_video_format_supported(enum AVPixelFormat format)
{
    for (int i = 0; zsx_video_formats[i] != AV_PIX_FMT_NONE; i++)
        if (zsx_video_formats[i] == format)
            return 1;
    return 0;
}

static int zsx_video_init(AVCodecContext *avctx)
{
    static const uint8_t magic[] = { 'Z', 'S', 'X', 'V', 1 };
    const AVPixFmtDescriptor *desc;

    if (av_codec_is_encoder(avctx->codec)) {
        desc = av_pix_fmt_desc_get(avctx->pix_fmt);
        if (!desc || !zsx_video_format_supported(avctx->pix_fmt) ||
            avctx->pix_fmt > UINT8_MAX)
            return AVERROR(EINVAL);
        avctx->extradata = av_mallocz(sizeof(magic) + 1 +
                                      AV_INPUT_BUFFER_PADDING_SIZE);
        if (!avctx->extradata)
            return AVERROR(ENOMEM);
        memcpy(avctx->extradata, magic, sizeof(magic));
        avctx->extradata[sizeof(magic)] = (uint8_t)avctx->pix_fmt;
        avctx->extradata_size = sizeof(magic) + 1;
        return 0;
    }

    if (avctx->extradata_size != (int)sizeof(magic) + 1 ||
        memcmp(avctx->extradata, magic, sizeof(magic)))
        return AVERROR_INVALIDDATA;
    avctx->pix_fmt = (enum AVPixelFormat)avctx->extradata[sizeof(magic)];
    desc = av_pix_fmt_desc_get(avctx->pix_fmt);
    if (!desc || !zsx_video_format_supported(avctx->pix_fmt))
        return AVERROR_INVALIDDATA;
    return 0;
}

static int zsx_video_encode(AVCodecContext *avctx, AVPacket *pkt,
                            const AVFrame *frame, int *got_packet)
{
    uint8_t *packed = NULL;
    uint8_t *coded = NULL;
    size_t coded_size = 0;
    int packed_size;
    int ret;

    if (frame->width != avctx->width || frame->height != avctx->height ||
        frame->format != avctx->pix_fmt)
        return AVERROR(EINVAL);

    packed_size = av_image_get_buffer_size(frame->format, frame->width,
                                           frame->height, 1);
    if (packed_size < 0)
        return packed_size;
    packed = av_malloc(packed_size ? (size_t)packed_size : 1);
    if (!packed)
        return AVERROR(ENOMEM);
    ret = av_image_copy_to_buffer(packed, packed_size,
                                  (const uint8_t **)frame->data,
                                  frame->linesize, frame->format,
                                  frame->width, frame->height, 1);
    if (ret < 0)
        goto done;

    ret = zsx_ffmpeg_encode(packed, (size_t)packed_size, &coded, &coded_size);
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
    av_free(packed);
    return ret;
}

static int zsx_video_decode(AVCodecContext *avctx, AVFrame *frame,
                            int *got_frame, AVPacket *avpkt)
{
    uint8_t *decoded = NULL;
    size_t decoded_size = 0;
    uint8_t *src_data[4] = { NULL };
    int src_linesize[4] = { 0 };
    int expected;
    int ret;

    if (avctx->width <= 0 || avctx->height <= 0 ||
        avctx->pix_fmt == AV_PIX_FMT_NONE)
        return AVERROR_INVALIDDATA;
    expected = av_image_get_buffer_size(avctx->pix_fmt, avctx->width,
                                        avctx->height, 1);
    if (expected < 0)
        return expected;

    ret = zsx_ffmpeg_decode(avpkt->data, (size_t)avpkt->size,
                            &decoded, &decoded_size);
    if (ret < 0 || decoded_size != (size_t)expected) {
        zsx_ffmpeg_free(decoded);
        return AVERROR_INVALIDDATA;
    }
    ret = ff_get_buffer(avctx, frame, 0);
    if (ret < 0)
        goto done;
    ret = av_image_fill_arrays(src_data, src_linesize, decoded, avctx->pix_fmt,
                               avctx->width, avctx->height, 1);
    if (ret < 0)
        goto done;
    av_image_copy(frame->data, frame->linesize,
                  (const uint8_t * const *)src_data, src_linesize,
                  avctx->pix_fmt, avctx->width, avctx->height);
    *got_frame = 1;
    ret = avpkt->size;

done:
    zsx_ffmpeg_free(decoded);
    return ret;
}

const FFCodec ff_zsx_video_encoder = {
    .p.name         = "zsx_video",
    CODEC_LONG_NAME("ZSX production video (lossless packed-frame chunks)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ZSX_VIDEO,
    .p.capabilities = AV_CODEC_CAP_DR1 | AV_CODEC_CAP_ENCODER_REORDERED_OPAQUE,
    .init           = zsx_video_init,
    FF_CODEC_ENCODE_CB(zsx_video_encode),
    .p.pix_fmts     = zsx_video_formats,
};

const FFCodec ff_zsx_video_decoder = {
    .p.name         = "zsx_video",
    CODEC_LONG_NAME("ZSX production video (lossless packed-frame chunks)"),
    .p.type         = AVMEDIA_TYPE_VIDEO,
    .p.id           = AV_CODEC_ID_ZSX_VIDEO,
    .p.capabilities = AV_CODEC_CAP_DR1,
    .init           = zsx_video_init,
    FF_CODEC_DECODE_CB(zsx_video_decode),
    .p.pix_fmts     = zsx_video_formats,
};