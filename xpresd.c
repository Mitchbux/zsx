#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned char* data;
    size_t length;
} Vec;

typedef enum {
    Ok,
    MemLimit,
    CorruptedData
} Result;

#define store32le(dst, idx, val) do { \
    dst[idx + 0] = (val) & 0xFF; \
    dst[idx + 1] = ((val) >> 8) & 0xFF; \
    dst[idx + 2] = ((val) >> 16) & 0xFF; \
    dst[idx + 3] = ((val) >> 24) & 0xFF; \
} while (0)

#define load16le(dst, src, idx) do { \
    dst = ((unsigned int)src[idx + 1] << 8) | (unsigned int)src[idx]; \
} while (0)

#define load32le(dst, src, idx) do { \
    dst = ((unsigned int)src[idx + 3] << 24) | ((unsigned int)src[idx + 2] << 16) | \
          ((unsigned int)src[idx + 1] << 8) | (unsigned int)src[idx]; \
} while (0)

Result decompress(const unsigned char* in_buf, size_t in_buf_len, unsigned char** out_buf, size_t* out_buf_len) {
    size_t out_idx = 0;
    size_t in_idx = 0;
    size_t nibble_idx = 0;

    size_t flags = 0;
    size_t flag_count = 0;

    size_t length;
    size_t offset;

    *out_buf = NULL;
    *out_buf_len = 0;

    while (in_idx < in_buf_len) {
        if (flag_count == 0) {
            if ((in_idx + 3) >= in_buf_len) {
                return MemLimit;
            }

            load32le(flags, in_buf, in_idx);
            in_idx += sizeof(unsigned int);
            flag_count = 32;
        }

        flag_count--;

        if ((flags & (1 << flag_count)) == 0) {
            if (in_idx >= in_buf_len) {
                return MemLimit;
            }
            *out_buf = realloc(*out_buf, (*out_buf_len + 1) * sizeof(unsigned char));
            (*out_buf)[out_idx] = in_buf[in_idx];

            in_idx += sizeof(unsigned char);
            out_idx += sizeof(unsigned char);
        } else {
            if ((in_idx + 1) >= in_buf_len) {
                return MemLimit;
            }

            load16le(length, in_buf, in_idx);
            in_idx += sizeof(unsigned short);

            offset = (length / 8) + 1;
            length = length % 8;

            if (length == 7) {
                if (nibble_idx == 0) {
                    if (in_idx >= in_buf_len) {
                        return MemLimit;
                    }

                    length = in_buf[in_idx] % 16;
                    nibble_idx = in_idx;
                    in_idx += sizeof(unsigned char);
                } else {
                    if (nibble_idx >= in_buf_len) {
                        return MemLimit;
                    }

                    length = in_buf[nibble_idx] / 16;
                    nibble_idx = 0;
                }

                if (length == 15) {
                    if (in_idx >= in_buf_len) {
                        return MemLimit;
                    }

                    length = in_buf[in_idx];

                    in_idx += sizeof(unsigned char);

                    if (length == 255) {
                        if ((in_idx + 1) >= in_buf_len) {
                            return MemLimit;
                        }

                        load16le(length, in_buf, in_idx);
                        in_idx += sizeof(unsigned short);

                        if (length == 0) {
                            load32le(length, in_buf, in_idx);
                            in_idx += sizeof(unsigned int);
                        }

                        if (length < 15 + 7) {
                            return CorruptedData;
                        }
                        length -= 15 + 7;
                    }
                    length += 15;
                }
                length += 7;
            }
            length += 3;

            for (size_t i = 0; i < length; i++) {
                if (offset > out_idx) {
                    return CorruptedData;
                }

                *out_buf = realloc(*out_buf, (*out_buf_len + 1) * sizeof(unsigned char));
                (*out_buf)[out_idx] = (*out_buf)[out_idx - offset];
                out_idx += sizeof(unsigned char);
            }
        }
    }

    *out_buf_len = out_idx;

    return Ok;
}

Result compress(const unsigned char* in_buf, size_t in_buf_len, unsigned char** out_buf, size_t* out_buf_len) {
    size_t in_idx = 0;
    size_t out_idx;
    size_t byte_left;

    size_t max_off;
    size_t match_off;

    size_t max_len;
    size_t best_len;

    unsigned int flags = 0;
    unsigned int flag_count = 0;
    size_t flag_out_off = 0;
    size_t nibble_index = 0;

    *out_buf = NULL;
    *out_buf_len = 0;

    *out_buf = realloc(*out_buf, sizeof(unsigned int));
    out_idx = sizeof(unsigned int);

    while (in_idx < in_buf_len) {
        int found = 0;
        byte_left = in_buf_len - in_idx;
        max_off = in_idx;

        size_t str1_off = in_idx;

        best_len = 2;
        match_off = 0;

        max_off = (8192 < max_off) ? 8192 : max_off;

        for (size_t offset = 1; offset <= max_off; offset++) {
            size_t len = 0;
            size_t str2_off = str1_off - offset;

            max_len = (8192 < byte_left) ? 8192 : byte_left;

            for (size_t i = 0; i < max_len; i++) {
                if (in_buf[str1_off + i] != in_buf[str2_off + i]) {
                    break;
                }
                len = i + 1;
            }

            if (len > best_len) {
                found = 1;
                best_len = len;
                match_off = offset;
            }
        }

        if (!found) {
            *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned char)));
            (*out_buf)[out_idx] = in_buf[in_idx];
            out_idx += sizeof(unsigned char);
            in_idx += sizeof(unsigned char);

            flags <<= 1;
            flag_count++;
            if (flag_count == 32) {
                store32le(*out_buf, flag_out_off, flags);
                flag_count = 0;
                flag_out_off = out_idx;
                *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned int)));
                (*out_buf)[out_idx] = 0;
                (*out_buf)[out_idx + 1] = 0;
                (*out_buf)[out_idx + 2] = 
                (*out_buf)[out_idx + 3] = 0;
                out_idx += sizeof(unsigned int);
            }
        } else {
            size_t match_len = best_len;
            size_t metadata_size = 0;

            match_len -= 3;
            match_off -= 1;

            if (match_len < 7) {
                unsigned short metadata = (match_off << 3) + match_len;
                *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned short)));
                (*out_buf)[out_idx] = metadata & 0xFF;
                (*out_buf)[out_idx + 1] = (metadata >> 8) & 0xFF;
                metadata_size += sizeof(unsigned short);
            } else {
                int has_extra_len = 0;
                unsigned short metadata = (match_off << 3) | 7;
                *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned short)));
                (*out_buf)[out_idx] = metadata & 0xFF;
                (*out_buf)[out_idx + 1] = (metadata >> 8) & 0xFF;
                metadata_size += sizeof(unsigned short);

                match_len -= 7;

                if (nibble_index == 0) {
                    nibble_index = out_idx;
                    if (match_len < 15) {
                        *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned char)));
                        (*out_buf)[out_idx + metadata_size] = match_len;
                        metadata_size += sizeof(unsigned char);
                    } else {
                        *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned char)));
                        (*out_buf)[out_idx + metadata_size] = 15;
                        metadata_size += sizeof(unsigned char);

                        has_extra_len = 1;
                    }
                } else {
                    if (match_len < 15) {
                        (*out_buf)[nibble_index] |= (match_len << 4) & 0xF0;
                        nibble_index = 0;
                    } else {
                        (*out_buf)[nibble_index] |= 0xF0;
                        nibble_index = 0;

                        has_extra_len = 1;
                    }
                }

                if (has_extra_len) {
                    match_len -= 15;

                    if (match_len < 255) {
                        *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned char)));
                        (*out_buf)[out_idx + metadata_size] = match_len;
                        metadata_size += sizeof(unsigned char);
                    } else {
                        *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned char)));
                        (*out_buf)[out_idx + metadata_size] = 255;
                        metadata_size += sizeof(unsigned char);

                        match_len += 7 + 15;

                        if (match_len < (1 << 16)) {
                            *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned short)));
                            (*out_buf)[out_idx + metadata_size] = match_len & 0xFF;
                            (*out_buf)[out_idx + metadata_size + 1] = (match_len >> 8) & 0xFF;
                            metadata_size += sizeof(unsigned short);
                        } else {
                            *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned short) + sizeof(unsigned int)));
                            (*out_buf)[out_idx + metadata_size] = 0;
                            (*out_buf)[out_idx + metadata_size + 1] = 0;
                            metadata_size += sizeof(unsigned short);
                            (*out_buf)[out_idx + metadata_size] = match_len & 0xFF;
                            (*out_buf)[out_idx + metadata_size + 1] = (match_len >> 8) & 0xFF;
                            (*out_buf)[out_idx + metadata_size + 2] = (match_len >> 16) & 0xFF;
                            (*out_buf)[out_idx + metadata_size + 3] = (match_len >> 24) & 0xFF;
                            metadata_size += sizeof(unsigned int);
                        }
                    }
                }
            }

            flags = (flags << 1) | 1;
            flag_count++;
            if (flag_count == 32) {
                store32le(*out_buf, flag_out_off, flags);
                flag_count = 0;
                flag_out_off = out_idx;
                *out_buf = realloc(*out_buf, (*out_buf_len + sizeof(unsigned int)));
                (*out_buf)[out_idx] = 0;
                (*out_buf)[out_idx + 1] = 0;
                (*out_buf)[out_idx + 2] = 0;
                (*out_buf)[out_idx + 3] = 0;
                out_idx += sizeof(unsigned int);
            }

            out_idx += metadata_size;
            in_idx += best_len;
        }
    }

    flags <<= (32 - flag_count);
    flags |= (1 << (32 - flag_count)) - 1;
    store32le(*out_buf, flag_out_off, flags);

    *out_buf_len = out_idx;

    return Ok;
}
