/*
 * Bounded coefficient-major RLE.  This is the native-C equivalent of the
 * experiment's coefficient_rle backend: plane, zig-zag frequency, block;
 * DC values are delta coded and every other coefficient is literal.
 */
#include "zsx_coefficient_rle.h"

#include <limits.h>
#include <string.h>

#define ZRLE_FREQ 64

static const uint8_t zsx_zigzag[ZRLE_FREQ] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

static int zsx_rle_samples(size_t blocks, size_t *samples)
{
    if (blocks > SIZE_MAX / ZRLE_FREQ)
        return -1;
    *samples = blocks * ZRLE_FREQ;
    return 0;
}

static int zsx_rle_put(uint8_t *out, size_t cap, size_t *at, uint64_t v)
{
    do {
        if (*at >= cap)
            return -1;
        out[(*at)++] = (v & ~UINT64_C(0x7f) ? 0x80 : 0) | (uint8_t)v;
        v >>= 7;
    } while (v);
    return 0;
}

static int zsx_rle_get(const uint8_t *in, size_t size, size_t *at,
                       uint64_t *value)
{
    uint64_t v = 0;
    int i;
    for (i = 0; i < 5; i++) {
        uint8_t b;
        if (*at >= size)
            return -1;
        b = in[(*at)++];
        v |= (uint64_t)(b & 0x7f) << (7 * i);
        if (!(b & 0x80)) {
            *value = v;
            return 0;
        }
    }
    return -1;
}

static uint64_t zsx_rle_zigzag(int64_t value)
{
    return value < 0 ? (uint64_t)(-(value + 1)) * 2 + 1 :
                       (uint64_t)value * 2;
}

int zsx_coefficient_rle_serialize(const int16_t *const *planes,
                                  const size_t *counts, size_t plane_count,
                                  uint8_t *output, size_t output_capacity,
                                  size_t *output_size)
{
    size_t plane, written = 0;

    if (!planes || !counts || !output || !output_size)
        return -1;
    for (plane = 0; plane < plane_count; plane++) {
        size_t frequency, count = counts[plane], samples;
        if (!planes[plane] || zsx_rle_samples(count, &samples))
            return -1;
        for (frequency = 0; frequency < ZRLE_FREQ; frequency++) {
            size_t position = 0;
            int32_t previous_dc = 0;
            while (position < count) {
                size_t run = 0;
                while (position + run < count) {
                    int32_t current = planes[plane][
                        (position + run) * ZRLE_FREQ + zsx_zigzag[frequency]];
                    int64_t value = frequency ? current :
                                    (int64_t)current - previous_dc;
                    if (value)
                        break;
                    if (!frequency)
                        previous_dc = current;
                    run++;
                }
                if (run > UINT64_MAX / 2 ||
                    zsx_rle_put(output, output_capacity, &written,
                                (uint64_t)run * 2 + (position + run < count)))
                    return -1;
                position += run;
                if (position == count)
                    break;
                {
                    int32_t current = planes[plane][
                        position * ZRLE_FREQ + zsx_zigzag[frequency]];
                    int64_t value = frequency ? current :
                                    (int64_t)current - previous_dc;
                    if (!frequency)
                        previous_dc = current;
                    if (zsx_rle_put(output, output_capacity, &written,
                                    zsx_rle_zigzag(value)))
                        return -1;
                }
                position++;
            }
        }
    }
    *output_size = written;
    return 0;
}

int zsx_coefficient_rle_parse(const uint8_t *input, size_t input_size,
                              const size_t *counts, size_t plane_count,
                              int16_t *output, size_t output_capacity,
                              size_t *output_samples)
{
    size_t plane, offset = 0, output_offset = 0;

    if (!input || !counts || !output || !output_samples)
        return -1;
    for (plane = 0; plane < plane_count; plane++) {
        size_t frequency, count = counts[plane], samples;
        if (zsx_rle_samples(count, &samples) ||
            samples > output_capacity - output_offset)
            return -1;
        memset(output + output_offset, 0, samples * sizeof(*output));
        /*
         * DC deltas span the difference between two int16 absolute values,
         * i.e. -65535..65535.  Decode to the cumulative value directly,
         * because an intermediate delta cannot itself always fit int16.
         */
        {
            size_t position = 0;
            int64_t dc = 0;
            while (position < count) {
                uint64_t token, run;
                if (zsx_rle_get(input, input_size, &offset, &token))
                    return -1;
                run = token >> 1;
                if (run > count - position)
                    return -1;
                while (run--)
                    output[output_offset + position++ * ZRLE_FREQ] = dc;
                if (token & 1) {
                    uint64_t encoded;
                    int64_t delta;
                    if (position >= count ||
                        zsx_rle_get(input, input_size, &offset, &encoded))
                        return -1;
                    delta = encoded & 1 ? -(int64_t)(encoded >> 1) - 1 :
                                          (int64_t)(encoded >> 1);
                    if (delta < -65535 || delta > 65535)
                        return -1;
                    dc += delta;
                    if (dc < INT16_MIN || dc > INT16_MAX)
                        return -1;
                    output[output_offset + position++ * ZRLE_FREQ] = dc;
                } else if (!position) {
                    return -1;
                }
            }
        }
        for (frequency = 1; frequency < ZRLE_FREQ; frequency++) {
            size_t position = 0;
            while (position < count) {
                uint64_t token, run;
                if (zsx_rle_get(input, input_size, &offset, &token))
                    return -1;
                run = token >> 1;
                if (run > count - position)
                    return -1;
                position += (size_t)run;
                if (token & 1) {
                    uint64_t encoded;
                    int64_t value;
                    if (position >= count ||
                        zsx_rle_get(input, input_size, &offset, &encoded))
                        return -1;
                    value = encoded & 1 ? -(int64_t)(encoded >> 1) - 1 :
                                          (int64_t)(encoded >> 1);
                    if (value < INT16_MIN || value > INT16_MAX)
                        return -1;
                    output[output_offset + position * ZRLE_FREQ +
                           zsx_zigzag[frequency]] = value;
                    position++;
                } else if (!position) {
                    return -1;
                }
            }
        }
        output_offset += samples;
    }
    if (offset != input_size)
        return -1;
    *output_samples = output_offset;
    return 0;
}