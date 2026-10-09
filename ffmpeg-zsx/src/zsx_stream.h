#ifndef ZSX_STREAM_H
#define ZSX_STREAM_H
#include <stddef.h>
#include <stdint.h>
/* ZSE1 v1 profiles. These are NOT ZSC2 archives or production ZSX1. */
enum {
    ZSX_PROFILE_BYTE16 = 1,
    ZSX_PROFILE_BYTE20 = 2,
    ZSX_PROFILE_CANDIDATE16 = 3,
    ZSX_PROFILE_CANDIDATE20 = 4
};
/* malloc-owned outputs; failures leave outputs untouched. */
int zsx_stream_encode(const uint8_t *, size_t, unsigned, uint8_t **, size_t *);
int zsx_stream_decode(const uint8_t *, size_t, uint8_t **, size_t *);
#endif
