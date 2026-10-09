/*
 * FFmpeg-facing bridge for the exact production zsx.c implementation.
 *
 * The production allocator stores the logical length immediately before the
 * returned pointer.  Keeping that detail here means the codec callbacks only
 * handle ordinary (pointer, size) buffers.
 */
#include "zsx_core_api.h"

#include <limits.h>
#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t byte;

void *zsx_alloc(size_t item_size, size_t n_item);
byte *zsx_encode(byte *data);
byte *zsx_decode(byte *data);

#define ZSX_CHUNK_SIZE 0xC00000u
#define ZSX_HEADER_SIZE 16u
#define ZSX_LENGTH(data) (*((size_t *)(data) - 1))
#define zsx_del(memory) free((size_t *)(memory) - 2)

/* The original predictor has many unchecked allocations. In library builds
 * its allocator unwinds to this boundary, freeing every partial allocation.
 * Thread-local scopes preserve concurrent native codec instances. */
typedef struct Allocation {
    void *ptr;
    size_t size;
    struct Allocation *next;
} Allocation;
typedef struct {
    jmp_buf failure;
    Allocation *head;
    size_t bytes;
} Scope;
static _Thread_local Scope *active;

void zsx_library_oom(void)
{
    /* Only entered by zsx_alloc while a bridge call owns the scope. */
    longjmp(active->failure, 1);
}
void *zsx_library_calloc(size_t n, size_t size)
{
    Allocation *entry;
    void *ptr;
    if (!active || (size && n > SIZE_MAX / size))
        return NULL;
    size *= n;
    if (size > 112u * 1024u * 1024u - active->bytes)
        zsx_library_oom();
    entry = malloc(sizeof(*entry));
    ptr = calloc(1, size);
    if (!entry || !ptr) {
        free(entry); free(ptr); zsx_library_oom();
    }
    entry->ptr = ptr; entry->size = size; entry->next = active->head;
    active->head = entry; active->bytes += size;
    return ptr;
}
void zsx_library_free(void *ptr)
{
    if (active) {
        Allocation **p = &active->head;
        while (*p && (*p)->ptr != ptr) p = &(*p)->next;
        if (*p) {
            Allocation *old = *p;
            *p = old->next; active->bytes -= old->size; free(old);
        }
    }
    free(ptr);
}
static int core_call(const uint8_t *input, size_t size, int decode,
                     uint8_t **out, size_t *out_size)
{
    /* Heap scope: fields modified after setjmp remain defined after longjmp. */
    Scope *scope = calloc(1, sizeof(*scope));
    Scope *previous = active;
    byte *buffer = NULL;
    int ret = ZSX_NOMEM;
    if (!scope) return ret;
    active = scope;
    if (!setjmp(scope->failure)) {
        buffer = zsx_alloc(1, ZSX_CHUNK_SIZE + ZSX_HEADER_SIZE + 1);
        if (size) memcpy(buffer, input, size);
        ZSX_LENGTH(buffer) = size;
        if (decode ? zsx_decode(buffer) != NULL : zsx_encode(buffer) != NULL) {
            *out = buffer; *out_size = ZSX_LENGTH(buffer); ret = 0;
        } else ret = ZSX_INVALID;
    }
    while (scope->head) {
        Allocation *entry = scope->head;
        scope->head = entry->next;
        if (ret || entry->ptr != (size_t *)*out - 2) free(entry->ptr);
        free(entry);
    }
    active = previous; free(scope);
    return ret;
}
static uint32_t get_u32(const byte *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int zsx_ffmpeg_encode(const uint8_t *input, size_t input_size,
                      uint8_t **out, size_t *out_size)
{
    if (!out || !out_size || (!input && input_size) ||
        input_size > ZSX_CHUNK_SIZE)
        return -1;

    /*
     * This is the same capacity used by the production file path.  It also
     * leaves the production decoder's terminating byte in bounds if this
     * packet is subsequently decoded.
     */
    return core_call(input, input_size, 0, out, out_size);
}

int zsx_ffmpeg_decode(const uint8_t *input, size_t input_size,
                      uint8_t **out, size_t *out_size)
{
    byte *buffer;
    uint32_t original, stream_size, miss_count, crc;
    size_t produced;

    if (!out || !out_size || !input ||
        input_size < ZSX_HEADER_SIZE ||
        input_size > (size_t)ZSX_CHUNK_SIZE + ZSX_HEADER_SIZE)
        return -1;

    original   = get_u32(input + 0);
    stream_size = get_u32(input + 4);
    miss_count = get_u32(input + 8);
    crc        = get_u32(input + 12);

    /*
     * These are the production zsx_decode() canonical checks performed
     * before it allocates its fixed working tables.  In particular, reject
     * hostile lengths here so a malformed packet can never make the core
     * write past the full chunk-sized destination.
     */
    if (original > ZSX_CHUNK_SIZE ||
        stream_size != input_size - ZSX_HEADER_SIZE ||
        crc != 0 ||
        (miss_count != UINT32_MAX && miss_count > original))
        return -1;
    if (miss_count == UINT32_MAX && stream_size != original)
        return -1;
    if (miss_count != UINT32_MAX && (original == 0 || stream_size < 5))
        return -1;

    {
        int ret = core_call(input, input_size, 1, &buffer, &produced);
        if (ret) return ret;
    }
    if (produced != original) {
        zsx_del(buffer);
        return -1;
    }
    *out = buffer;
    *out_size = produced;
    return 0;
}

void zsx_ffmpeg_free(uint8_t *buffer)
{
    if (buffer)
        zsx_del(buffer);
}

static uint8_t *owned_copy(const uint8_t *data, size_t size)
{
    size_t *p = malloc(2 * sizeof(size_t) + size);
    if (!p) return NULL;
    p[0] = 1; p[1] = size;
    if (size) memcpy(p + 2, data, size);
    return (uint8_t *)(p + 2);
}

int zsx_ffmpeg_encode_best(const uint8_t *input, size_t size,
                           uint8_t **out, size_t *out_size)
{
    uint8_t *best = NULL;
    size_t best_size = 0;
    int ret;
    if (!out || !out_size) return ZSX_INVALID;
    ret = zsx_ffmpeg_encode(input, size, &best, &best_size);
    if (ret) return ret;
    /* Sequential trials bound memory independently of the candidate count.
     * 20-bit profiles are supported for explicit callers, not default trials. */
    for (unsigned profile = ZSX_PROFILE_BYTE16;
         profile <= ZSX_PROFILE_CANDIDATE16; profile += 2) {
        uint8_t *trial = NULL;
        size_t trial_size = 0;
        ret = zsx_stream_encode(input, size, profile, &trial, &trial_size);
        if (!ret && trial_size < best_size) {
            uint8_t *copy = owned_copy(trial, trial_size);
            if (copy) {
                zsx_ffmpeg_free(best); best = copy; best_size = trial_size;
            }
        }
        free(trial);
        /* OOM on an optional trial retains the already-valid baseline. */
    }
    *out = best; *out_size = best_size;
    return 0;
}
int zsx_ffmpeg_encode_selected(const uint8_t *input, size_t size,
                               unsigned profile, uint8_t **out, size_t *out_size)
{
    uint8_t *encoded = NULL, *copy;
    size_t encoded_size = 0;
    int ret;
    if (!profile) return zsx_ffmpeg_encode_best(input, size, out, out_size);
    if (!out || !out_size || profile > ZSX_PROFILE_CANDIDATE20) return ZSX_INVALID;
    ret = zsx_stream_encode(input, size, profile, &encoded, &encoded_size);
    if (ret) return ret;
    /* Keep the established chunk-capacity bound for incompressible input. */
    if (encoded_size > size + 16) {
        free(encoded);
        return zsx_ffmpeg_encode(input, size, out, out_size);
    }
    copy = owned_copy(encoded, encoded_size);
    free(encoded);
    if (!copy) return ZSX_NOMEM;
    *out = copy; *out_size = encoded_size;
    return 0;
}

int zsx_ffmpeg_decode_profile(const uint8_t *input, size_t size,
                              uint8_t **out, size_t *out_size)
{
    uint8_t *decoded = NULL, *copy;
    size_t decoded_size = 0;
    int ret;
    if (!input || !out || !out_size) return ZSX_INVALID;
    if (size < 4 || memcmp(input, "ZSE1", 4))
        return zsx_ffmpeg_decode(input, size, out, out_size);
    ret = zsx_stream_decode(input, size, &decoded, &decoded_size);
    if (ret) return ret;
    copy = owned_copy(decoded, decoded_size); free(decoded);
    if (!copy) return ZSX_NOMEM;
    *out = copy; *out_size = decoded_size;
    return 0;
}