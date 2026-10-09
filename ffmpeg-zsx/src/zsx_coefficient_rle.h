#ifndef ZSX_COEFFICIENT_RLE_H
#define ZSX_COEFFICIENT_RLE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Coefficient-major, zig-zag ordered RLE used by zsx_dct_video.  The
 * functions never allocate; the caller supplies bounded buffers.  A zero
 * return is success, every non-zero return denotes malformed input or a
 * capacity/bounds failure.
 */
int zsx_coefficient_rle_serialize(const int16_t *const *planes,
                                  const size_t *counts, size_t plane_count,
                                  uint8_t *output, size_t output_capacity,
                                  size_t *output_size);
int zsx_coefficient_rle_parse(const uint8_t *input, size_t input_size,
                              const size_t *counts, size_t plane_count,
                              int16_t *output, size_t output_capacity,
                              size_t *output_samples);

#endif