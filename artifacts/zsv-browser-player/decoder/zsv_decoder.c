/*
 * ZSV2 browser ABI.  This is intentionally a very small FFmpeg front end:
 * all packet validation and ZSX decoding remains in ffmpeg-zsx's custom
 * codecs.  No stock codec is selected for a ZSV2 stream.
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
EM_ASYNC_JS(int, stream_read, (uint8_t *buf, int n), {
  try {
    const bytes = await Module.zsvRead(n);
    if (!bytes.length) return -541478725; // AVERROR_EOF
    HEAPU8.set(bytes, buf);
    return bytes.length;
  } catch (error) {
    Module.streamError = String(error);
    return -5; // AVERROR(EIO)
  }
});
#endif

typedef struct {
  AVFormatContext *fmt; AVIOContext *io; uint8_t *io_buf;
  AVCodecContext *vc, *ac; int vs, as; AVPacket *pkt; AVFrame *frame;
  uint8_t *input, *base; size_t input_size; int eof, video_done, audio_done, streaming;
  char error[192];
} ZSV;

static void zsv_release(ZSV *z) {
  if (!z) return;
  avcodec_free_context(&z->vc);
  avcodec_free_context(&z->ac);
  av_frame_free(&z->frame);
  av_packet_free(&z->pkt);
  avformat_close_input(&z->fmt);
  if (z->io) {
    av_freep(&z->io->buffer);
    avio_context_free(&z->io);
  } else {
    av_freep(&z->io_buf);
  }
  av_free(z);
}

static void err(ZSV *z, int e, const char *where) {
  char b[96]; av_strerror(e, b, sizeof b);
  snprintf(z->error, sizeof z->error, "%s: %s", where, b);
}
static int read_cb(void *opaque, uint8_t *buf, int n) {
  ZSV *z = opaque;
#ifdef __EMSCRIPTEN__
  if (z->streaming) return stream_read(buf, n);
#endif
  size_t left = z->input_size - (size_t)(z->input - z->base);
  if (!left) return AVERROR_EOF;
  if ((size_t)n > left) n = (int)left;
  memcpy(buf, z->input, n); z->input += n; return n;
}
static int64_t seek_cb(void *opaque, int64_t off, int whence) {
  ZSV *z = opaque; uint8_t *base = z->base;
  if (whence == AVSEEK_SIZE) return (int64_t)z->input_size;
  int64_t p = (whence == SEEK_CUR) ? z->input - base : (whence == SEEK_END ? z->input_size : 0);
  p += off; if (p < 0 || (uint64_t)p > z->input_size) return AVERROR(EINVAL);
  z->input = base + p; return p;
}
static int custom(AVCodecContext *c, const char *name) {
  return c && c->codec && !strcmp(c->codec->name, name);
}
static int open_input(uint8_t *data, int size, int streaming) {
  ZSV *z; const AVInputFormat *nut; int r, i;
  if (!streaming && (!data || size <= 0)) return 0;
  z = av_mallocz(sizeof *z); if (!z) return 0;
  z->input = z->base = data; z->input_size = size; z->vs = z->as = -1;
  z->streaming = streaming;
  z->io_buf = av_malloc(32768); if (!z->io_buf) goto fail;
  z->io = avio_alloc_context(z->io_buf,32768,0,z,read_cb,0,streaming ? NULL : seek_cb);
  if (!z->io) goto fail;
  z->io_buf = NULL;
  z->fmt = avformat_alloc_context(); if (!z->fmt) goto fail;
  z->fmt->pb = z->io; z->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  nut = av_find_input_format("nut"); r = avformat_open_input(&z->fmt, NULL, nut, NULL);
  if (r < 0) { err(z,r,"NUT"); goto fail; }
  r = avformat_find_stream_info(z->fmt, NULL); if (r < 0) { err(z,r,"streams"); goto fail; }
  { AVDictionaryEntry *profile = av_dict_get(z->fmt->metadata,"zsv_profile",NULL,0);
    if (!profile || strcmp(profile->value,"2")) goto fail; }
  for (i=0;i<(int)z->fmt->nb_streams;i++) {
    AVStream *s=z->fmt->streams[i]; const AVCodec *co=avcodec_find_decoder(s->codecpar->codec_id);
    if (s->codecpar->codec_type==AVMEDIA_TYPE_VIDEO && z->vs<0) z->vs=i;
    if (s->codecpar->codec_type==AVMEDIA_TYPE_AUDIO && z->as<0) z->as=i;
    if (co && ((!strcmp(co->name,"zsx_dct_video") && s->codecpar->codec_type==AVMEDIA_TYPE_VIDEO) ||
               (!strcmp(co->name,"zsx_mdct_audio") && s->codecpar->codec_type==AVMEDIA_TYPE_AUDIO))) {
      AVCodecContext **dst = s->codecpar->codec_type==AVMEDIA_TYPE_VIDEO ? &z->vc : &z->ac;
      *dst=avcodec_alloc_context3(co); if (!*dst || avcodec_parameters_to_context(*dst,s->codecpar)<0 ||
          avcodec_open2(*dst,co,NULL)<0) goto fail;
    }
  }
  if (z->fmt->nb_streams!=2 || z->vs<0 || z->as<0 || !z->vc || !z->ac) goto fail;
  z->pkt=av_packet_alloc(); z->frame=av_frame_alloc();
  if (!z->pkt || !z->frame) goto fail;
  return (int)(intptr_t)z;
fail: zsv_release(z); return 0;
}
int zsv_open(uint8_t *data, int size) { return open_input(data, size, 0); }
int zsv_open_stream(void) { return open_input(NULL, 0, 1); }
const char *zsv_error(int h) { return h ? ((ZSV *)(intptr_t)h)->error : "invalid handle"; }
int zsv_next(int h, int *kind, int *w, int *height, int *channels, int *rate,
              int *samples, int *pts_millis, uint8_t **out, int *out_size) {
  ZSV *z=(ZSV *)(intptr_t)h; int r; AVCodecContext *c; AVPacket *p=z->pkt;
  if (!z||!kind||!out||!out_size) return -1;
  for (;;) {
    r=av_read_frame(z->fmt,p);
    if (r==AVERROR_EOF) return 0;
    if (r<0) { err(z,r,"demux"); return -1; }
    c=p->stream_index==z->vs?z->vc:(p->stream_index==z->as?z->ac:NULL);
    if (!c || (r=avcodec_send_packet(c,p))<0) { av_packet_unref(p); if(r<0){err(z,r,"packet");return -1;} continue; }
    av_packet_unref(p); r=avcodec_receive_frame(c,z->frame);
    if (r==AVERROR(EAGAIN)||r==AVERROR_EOF) continue;
    if(r<0){err(z,r,"decode");return -1;}
    if(c==z->vc) {
      int y, off=0; int sz=z->frame->width*z->frame->height*3/2; uint8_t *q=av_malloc(sz);
      if(!q)return -1; for(y=0;y<z->frame->height;y++){memcpy(q+off,z->frame->data[0]+y*z->frame->linesize[0],z->frame->width);off+=z->frame->width;}
      for(y=0;y<z->frame->height/2;y++){memcpy(q+off,z->frame->data[1]+y*z->frame->linesize[1],z->frame->width/2);off+=z->frame->width/2;}
      for(y=0;y<z->frame->height/2;y++){memcpy(q+off,z->frame->data[2]+y*z->frame->linesize[2],z->frame->width/2);off+=z->frame->width/2;}
      *kind=1;*w=z->frame->width;*height=z->frame->height;*channels=0;*rate=0;*samples=0;
      *pts_millis = z->frame->best_effort_timestamp == AV_NOPTS_VALUE ? -1 : (int)av_rescale_q(z->frame->best_effort_timestamp, z->fmt->streams[z->vs]->time_base, (AVRational){1,1000});
      *out=q;*out_size=sz;return 1;
    } else {
      int sz=z->frame->nb_samples*c->ch_layout.nb_channels*4; uint8_t *q=av_malloc(sz);
      if(!q)return -1; memcpy(q,z->frame->extended_data[0],sz);
      *kind=2;*w=0;*height=0;*channels=c->ch_layout.nb_channels;*rate=c->sample_rate;*samples=z->frame->nb_samples;
      *pts_millis = z->frame->best_effort_timestamp == AV_NOPTS_VALUE ? -1 : (int)av_rescale_q(z->frame->best_effort_timestamp, z->fmt->streams[z->as]->time_base, (AVRational){1,1000});
      *out=q;*out_size=sz;return 1;
    }
  }
}
void zsv_free(void *p){av_free(p);}
void zsv_close(int h){zsv_release((ZSV *)(intptr_t)h);}