/*
 * Small, allocation-safe bridge around the production ZSX1 core.
 *
 * zsx_core.c is generated verbatim from the repository-root zsx.c by
 * scripts/build.sh.  The bridge deliberately does not reimplement any of
 * the predictor or range coder.
 */
#ifndef ZSX_CORE_API_H
#define ZSX_CORE_API_H

#include <stddef.h>
#include <stdint.h>
#include "zsx_stream.h"

enum { ZSX_ENTROPY_ZSX1 = 0, ZSX_VIDEO_LEGACY = 1, ZSX_VIDEO_STREAM = 2 };
enum { ZSX_INVALID = -1, ZSX_NOMEM = -2 };
/* Internal hooks used only by zsx.c compiled with -DZSX_LIBRARY. */
void *zsx_library_calloc(size_t, size_t);
void zsx_library_free(void *);
void zsx_library_oom(void);

/*
 * The returned buffer is owned by the production core allocator and must be
 * released with zsx_ffmpeg_free().  On failure these functions return a
 * negative value and leave *out untouched.
 */
int zsx_ffmpeg_encode(const uint8_t *input, size_t input_size,
                      uint8_t **out, size_t *out_size);
int zsx_ffmpeg_decode(const uint8_t *input, size_t input_size,
                      uint8_t **out, size_t *out_size);
void zsx_ffmpeg_free(uint8_t *buffer);

/* Version-2 video only: choose the smallest complete ZSX1/ZSE1 chunk.
 * Decode dispatch is explicit at the video-version boundary; never use this
 * for version-1 video/audio or uploaded ZSC2/pass-count archives. */
int zsx_ffmpeg_encode_best(const uint8_t *, size_t, uint8_t **, size_t *);
/* 0 selects size-based trials; 1..4 choose a single ZSE1 profile. */
int zsx_ffmpeg_encode_selected(const uint8_t *, size_t, unsigned, uint8_t **, size_t *);
int zsx_ffmpeg_decode_profile(const uint8_t *, size_t, uint8_t **, size_t *);

#endif