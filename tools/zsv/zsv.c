/* ZSV B&W&R converter. Uses FFmpeg libraries directly, not ffmpeg.exe.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
#endif

typedef struct Track {
    AVCodecContext *dec, *enc;
    AVStream *in, *out;
    AVFilterGraph *graph;
    AVFilterContext *source, *sink;
    struct SwsContext *sws;
    int video;
} Track;
static AVFormatContext *input, *output;
static Track video, audio;
static int64_t origin, silent_pts;
static char temporary[4096];
#ifdef _WIN32
static wchar_t *wide_path(const char *path) {
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
    wchar_t *value = n > 0 ? malloc(n * sizeof(wchar_t)) : NULL;
    if (value) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, value, n);
    return value;
}
#endif

static void check(int result, const char *operation) {
    if (result < 0) {
        char text[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, text, sizeof(text));
        fprintf(stderr, "zsv: %s: %s\n", operation, text);
        exit(1);
    }
}
static void cleanup(void) {
    Track *tracks[] = { &video, &audio };
    for (int i = 0; i < 2; i++) {
        avcodec_free_context(&tracks[i]->dec);
        avcodec_free_context(&tracks[i]->enc);
        avfilter_graph_free(&tracks[i]->graph);
        sws_freeContext(tracks[i]->sws);
    }
    avformat_close_input(&input);
    if (output) { avio_closep(&output->pb); avformat_free_context(output); }
    /* Never leave a failed conversion disguised as a valid .zsv. */
    if (temporary[0]) {
#ifdef _WIN32
        wchar_t *path = wide_path(temporary);
        if (path) { DeleteFileW(path); free(path); }
#else
        remove(temporary);
#endif
    }
}
static void encode(Track *t, AVFrame *frame) {
    check(avcodec_send_frame(t->enc, frame), "send encoder frame");
    AVPacket *packet = av_packet_alloc();
    if (!packet) check(AVERROR(ENOMEM), "packet allocation");
    int result;
    while ((result = avcodec_receive_packet(t->enc, packet)) >= 0) {
        av_packet_rescale_ts(packet, t->enc->time_base, t->out->time_base);
        packet->stream_index = t->out->index;
        check(av_interleaved_write_frame(output, packet), "write ZSV packet");
        av_packet_unref(packet);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) check(result, "receive encoder packet");
    av_packet_free(&packet);
}
static float clamp(float value) { return fmaxf(0, fminf(1, value)); }
static void red_only(AVFrame *f) {
    check(av_frame_make_writable(f), "writable RGB frame");
    for (int y = 0; y < f->height; y++) {
        unsigned char *row = f->data[0] + y * f->linesize[0];
        for (int x = 0; x < f->width; x++, row += 3) {
            float r = row[0], g = row[1], b = row[2];
            float hi = fmaxf(r, fmaxf(g, b)), lo = fminf(r, fminf(g, b));
            float delta = hi - lo;
            float hue = delta > 0 ? fabsf(60 * (g - b) / delta) : 360;
            float sat = hi > 0 ? delta / hi : 0;
            float mask = r == hi ? clamp((15 - hue) / 5) * clamp((sat - .35f) / .15f) : 0;
            float gray = .299f * r + .587f * g + .114f * b;
            row[0] = (unsigned char)(gray + mask * (r - gray));
            row[1] = (unsigned char)(gray + mask * (g - gray));
            row[2] = (unsigned char)(gray + mask * (b - gray));
        }
    }
}
static void silence_until(int64_t video_pts) {
    if (audio.dec) return;
    while (av_compare_ts(silent_pts, audio.enc->time_base, video_pts, video.enc->time_base) < 0) {
        AVFrame *f = av_frame_alloc();
        if (!f) check(AVERROR(ENOMEM), "silence allocation");
        f->format = audio.enc->sample_fmt; f->sample_rate = audio.enc->sample_rate;
        f->nb_samples = audio.enc->frame_size; f->pts = silent_pts;
        check(av_channel_layout_copy(&f->ch_layout, &audio.enc->ch_layout), "silence layout");
        check(av_frame_get_buffer(f, 0), "silence buffer");
        check(av_samples_set_silence(f->data, 0, f->nb_samples, 2, f->format), "silence samples");
        encode(&audio, f); silent_pts += f->nb_samples; av_frame_free(&f);
    }
}
static void drain_filter(Track *t) {
    AVFrame *f = av_frame_alloc();
    if (!f) check(AVERROR(ENOMEM), "filter frame");
    int result;
    while ((result = av_buffersink_get_frame(t->sink, f)) >= 0) {
        f->pts = av_rescale_q(f->pts, av_buffersink_get_time_base(t->sink), t->enc->time_base);
        if (t->video) {
            red_only(f);
            AVFrame *yuv = av_frame_alloc();
            if (!yuv) check(AVERROR(ENOMEM), "YUV frame");
            yuv->width = 270; yuv->height = 480; yuv->format = AV_PIX_FMT_YUV420P; yuv->pts = f->pts;
            check(av_frame_get_buffer(yuv, 32), "YUV buffer");
            t->sws = sws_getCachedContext(t->sws, 270, 480, AV_PIX_FMT_RGB24,
                270, 480, AV_PIX_FMT_YUV420P, SWS_BILINEAR, NULL, NULL, NULL);
            if (!t->sws) check(AVERROR(ENOMEM), "RGB to YUV");
            check(sws_scale(t->sws, (const uint8_t *const *)f->data, f->linesize,
                0, 480, yuv->data, yuv->linesize), "convert red frame");
            encode(t, yuv); silence_until(yuv->pts + 1); av_frame_free(&yuv);
        } else encode(t, f);
        av_frame_unref(f);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) check(result, "filter output");
    av_frame_free(&f);
}
static void init_filter(Track *t, AVFrame *f) {
    char args[512], description[512], layout[128];
    t->graph = avfilter_graph_alloc();
    if (!t->graph) check(AVERROR(ENOMEM), "filter graph");
    if (t->video) {
        AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : (AVRational){1,1};
        snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
            f->width, f->height, f->format, t->in->time_base.num, t->in->time_base.den, sar.num, sar.den);
        snprintf(description, sizeof(description),
            "scale=270:480:force_original_aspect_ratio=increase,crop=270:480,setsar=1,fps=25:start_time=0,format=rgb24");
    } else {
        check(av_channel_layout_describe(&f->ch_layout, layout, sizeof(layout)), "audio layout");
        snprintf(args, sizeof(args), "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%s",
            t->in->time_base.num, t->in->time_base.den, f->sample_rate, av_get_sample_fmt_name(f->format), layout);
        snprintf(description, sizeof(description),
            "aresample=44100:async=1:first_pts=0,aformat=sample_fmts=flt:channel_layouts=stereo,asetnsamples=n=%d:p=1",
            t->enc->frame_size);
    }
    check(avfilter_graph_create_filter(&t->source, avfilter_get_by_name(t->video ? "buffer" : "abuffer"),
        "in", args, NULL, t->graph), "input filter");
    check(avfilter_graph_create_filter(&t->sink, avfilter_get_by_name(t->video ? "buffersink" : "abuffersink"),
        "out", NULL, NULL, t->graph), "output filter");
    AVFilterInOut *in = avfilter_inout_alloc(), *out = avfilter_inout_alloc();
    if (!in || !out) check(AVERROR(ENOMEM), "filter endpoints");
    in->name = av_strdup("out"); in->filter_ctx = t->sink; in->pad_idx = 0;
    out->name = av_strdup("in"); out->filter_ctx = t->source; out->pad_idx = 0;
    check(avfilter_graph_parse_ptr(t->graph, description, &in, &out, NULL), "parse conversion filters");
    avfilter_inout_free(&in); avfilter_inout_free(&out);
    check(avfilter_graph_config(t->graph, NULL), "configure filters");
}
static void decode(Track *t, AVPacket *packet) {
    check(avcodec_send_packet(t->dec, packet), "send decoder packet");
    AVFrame *f = av_frame_alloc();
    if (!f) check(AVERROR(ENOMEM), "decoder frame");
    int result;
    while ((result = avcodec_receive_frame(t->dec, f)) >= 0) {
        f->pts = f->best_effort_timestamp;
        if (f->pts == AV_NOPTS_VALUE) check(AVERROR_INVALIDDATA, "input has no frame timestamps");
        f->pts -= av_rescale_q(origin, AV_TIME_BASE_Q, t->in->time_base);
        if (!t->graph) init_filter(t, f);
        check(av_buffersrc_add_frame_flags(t->source, f, AV_BUFFERSRC_FLAG_KEEP_REF), "feed filter");
        drain_filter(t); av_frame_unref(f);
    }
    if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) check(result, "decode input");
    av_frame_free(&f);
}
static void open_track(Track *t, int index, int is_video) {
    t->video = is_video;
    if (index >= 0) {
        t->in = input->streams[index];
        const AVCodec *decoder = avcodec_find_decoder(t->in->codecpar->codec_id);
        if (!decoder) check(AVERROR_DECODER_NOT_FOUND, "unsupported input codec");
        t->dec = avcodec_alloc_context3(decoder);
        if (!t->dec) check(AVERROR(ENOMEM), "decoder allocation");
        check(avcodec_parameters_to_context(t->dec, t->in->codecpar), "input parameters");
        t->dec->pkt_timebase = t->in->time_base;
        check(avcodec_open2(t->dec, decoder, NULL), "open decoder");
    }
    const AVCodec *encoder = avcodec_find_encoder_by_name(is_video ? "zsx_dct_video" : "zsx_mdct_audio");
    if (!encoder) check(AVERROR_ENCODER_NOT_FOUND, "FFmpeg lacks ZSV codecs");
    t->enc = avcodec_alloc_context3(encoder);
    if (!t->enc) check(AVERROR(ENOMEM), "encoder allocation");
    if (is_video) {
        t->enc->width = 270; t->enc->height = 480; t->enc->pix_fmt = AV_PIX_FMT_YUV420P;
        t->enc->time_base = (AVRational){1,25}; t->enc->framerate = (AVRational){25,1};
        t->enc->sample_aspect_ratio = (AVRational){1,1};
        check(av_opt_set_int(t->enc->priv_data, "zsx_quality", 70, 0), "ZSV quality");
        check(av_opt_set_int(t->enc->priv_data, "zsx_entropy", 3, 0), "ZSV entropy");
    } else {
        t->enc->sample_fmt = AV_SAMPLE_FMT_FLT; t->enc->sample_rate = 44100;
        t->enc->time_base = (AVRational){1,44100};
        av_channel_layout_default(&t->enc->ch_layout, 2);
        check(av_opt_set_double(t->enc->priv_data, "zsx_step", .016, 0), "ZSV audio quality");
    }
    if (output->oformat->flags & AVFMT_GLOBALHEADER) t->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    check(avcodec_open2(t->enc, encoder, NULL), "open ZSV encoder");
    t->out = avformat_new_stream(output, NULL);
    if (!t->out) check(AVERROR(ENOMEM), "output stream");
    t->out->time_base = t->enc->time_base;
    check(avcodec_parameters_from_context(t->out->codecpar, t->enc), "output parameters");
}
static int run(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "zsv - B&W&R video converter\nUsage: zsv.exe INPUT OUTPUT.zsv\n"
            "270x480 portrait, 25 fps, red accents on grayscale, stereo audio.\n"
            "Existing outputs are never overwritten. FFmpeg libraries are built in.\n");
        return argc == 2 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h")) ? 0 : 2;
    }
    if (avio_check(argv[2], AVIO_FLAG_READ) >= 0) { fprintf(stderr, "Output already exists.\n"); return 2; }
    if (strlen(argv[2]) + 6 >= sizeof(temporary)) return 2;
    snprintf(temporary, sizeof(temporary), "%s.part", argv[2]);
    if (avio_check(temporary, AVIO_FLAG_READ) >= 0) { temporary[0] = 0; fprintf(stderr, "Temporary output already exists.\n"); return 2; }
    atexit(cleanup);
    av_log_set_level(AV_LOG_WARNING);
    check(avformat_open_input(&input, argv[1], NULL, NULL), "open input");
    check(avformat_find_stream_info(input, NULL), "read input streams");
    int vi = av_find_best_stream(input, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
    check(vi, "find video");
    int ai = av_find_best_stream(input, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    if (ai < 0 && ai != AVERROR_STREAM_NOT_FOUND) check(ai, "find audio decoder");
    origin = input->start_time == AV_NOPTS_VALUE ? 0 : input->start_time;
    check(avformat_alloc_output_context2(&output, NULL, "nut", temporary), "create ZSV container");
    open_track(&video, vi, 1); open_track(&audio, ai, 0);
    av_dict_set(&output->metadata, "zsv_profile", "2", 0);
    av_dict_set(&output->metadata, "comment", "B&W&R: grayscale with red accents", 0);
    check(avio_open(&output->pb, temporary, AVIO_FLAG_WRITE), "open output");
    check(avformat_write_header(output, NULL), "write header");
    AVPacket *packet = av_packet_alloc();
    if (!packet) check(AVERROR(ENOMEM), "input packet");
    int result;
    while ((result = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == vi) decode(&video, packet);
        else if (packet->stream_index == ai) decode(&audio, packet);
        av_packet_unref(packet);
    }
    if (result != AVERROR_EOF) check(result, "read input");
    av_packet_free(&packet);
    decode(&video, NULL);
    if (!video.graph) check(AVERROR_INVALIDDATA, "input contains no video frames");
    check(av_buffersrc_add_frame_flags(video.source, NULL, 0), "flush video filter"); drain_filter(&video);
    if (audio.dec) {
        decode(&audio, NULL);
        if (!audio.graph) check(AVERROR_INVALIDDATA, "input contains no audio frames");
        check(av_buffersrc_add_frame_flags(audio.source, NULL, 0), "flush audio filter"); drain_filter(&audio);
    }
    encode(&video, NULL); encode(&audio, NULL);
    check(av_write_trailer(output), "finish ZSV");
    check(avio_closep(&output->pb), "close output");
#ifdef _WIN32
    wchar_t *from = wide_path(temporary), *to = wide_path(argv[2]);
    if (!from || !to || !MoveFileW(from, to)) check(AVERROR(EIO), "rename output");
    free(from); free(to);
#else
    if (rename(temporary, argv[2])) check(AVERROR(errno), "rename output");
#endif
    temporary[0] = 0;
    fprintf(stderr, "Created %s\n", argv[2]);
    return 0;
}
#ifdef _WIN32
int wmain(int argc, wchar_t **wide) {
    char **argv = calloc((size_t)argc + 1, sizeof(*argv));
    if (!argv) return 1;
    for (int i = 0; i < argc; i++) {
        int size = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, NULL, 0, NULL, NULL);
        if (!size || !(argv[i] = malloc(size))) return 1;
        WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, argv[i], size, NULL, NULL);
    }
    int result = run(argc, argv);
    for (int i = 0; i < argc; i++) free(argv[i]);
    free(argv); return result;
}
#else
int main(int argc, char **argv) { return run(argc, argv); }
#endif
