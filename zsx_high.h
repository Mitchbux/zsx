#ifndef ZSX_HIGH_H
#define ZSX_HIGH_H

/*
 * ZHP1 dictionary preprocessing, independent of the baseline predictor.
 * All integers are canonical unsigned LEB128 unless otherwise noted.
 * Wire format:
 *   "ZHP1", mode byte (0=frequency hash, 1=sequential index),
 *   original length, original CRC32 (4 bytes little endian), dictionary count,
 *   dictionary entries: word length, hash byte [0,127], exact word bytes,
 *   tokens, container CRC32 (4 bytes little endian, covers all preceding bytes).
 * Tokens: 0..127 = occupied hash-slot hit (mode 0 only);
 *   128 = literal length [1,65535], literal bytes;
 *   129 = DEFINE/REPLACE, dictionary index (mode 0 only);
 *   130 = dictionary index (mode 1 only).
 * DEFINE/REPLACE emits the indexed word and installs it in its declared slot.
 * A definition of the word already in that slot is noncanonical and rejected.
 * Indices are zero based. No terminator; declared output length and exact
 * token consumption must both match. Duplicate dictionary words/hashes are
 * permitted. Checksums are corruption detection, not authentication.
 *
 * Encoder limits: 12 MiB input, first 65536 distinct eligible words tracked,
 * first 4096 tracked words occurring at least three times selected, in order
 * of first occurrence. Words are maximal ASCII-letter runs, length 3..128.
 * Longer runs remain literals (never split into dictionary words).
 * Frequency ranks include ALL input bytes, ties resolved by byte value.
 * Hash uses the three best-ranked character occurrences, their positions and
 * word length. Selection/table probing is deterministic and memory bounded.
 * Decoder also limits original output to 12 MiB and dictionary to 4096.
 *
 * Success returns 1 and malloc-owned *out; caller must free it. Failure/no
 * candidate returns 0, *out=NULL, *out_len=0. Encode statistics are reset on
 * failure. Input may be NULL only when length is zero. Output parameters must
 * be non-NULL and distinct from the input storage. No global mutable state.
 */
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define ZSX_HIGH_MAX_INPUT ((size_t)12 * 1024 * 1024)
#define ZSX_HIGH_MAX_WORDS 65536u
#define ZSX_HIGH_TABLE_SIZE 131072u
#define ZSX_HIGH_MAX_DICT 4096u

struct zsx_high_stats {
    size_t words, dictionary_entries, hash_hits, definitions, collisions;
};

struct zsx_h_buf { unsigned char *p; size_t n, cap; };
struct zsx_h_word { size_t off; uint32_t count; uint16_t len, id; };
struct zsx_h_entry { const unsigned char *p; size_t len; unsigned char hash; };

static inline int zsx_h_letter(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

static inline uint32_t zsx_h_crc(const unsigned char *p, size_t n)
{
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    unsigned j;
    for (i = 0; i < n; ++i) {
        crc ^= p[i];
        for (j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (UINT32_C(0xedb88320) & (0u - (crc & 1u)));
    }
    return ~crc;
}

static inline int zsx_h_put(struct zsx_h_buf *b, const void *p, size_t n)
{
    size_t cap;
    unsigned char *next;
    if (n > SIZE_MAX - b->n) return 0;
    if (b->n + n > b->cap) {
        cap = b->cap ? b->cap : 256;
        while (cap < b->n + n) {
            if (cap > SIZE_MAX / 2) { cap = b->n + n; break; }
            cap *= 2;
        }
        next = (unsigned char *)realloc(b->p, cap);
        if (!next) return 0;
        b->p = next; b->cap = cap;
    }
    if (n) memcpy(b->p + b->n, p, n);
    b->n += n;
    return 1;
}

static inline int zsx_h_byte(struct zsx_h_buf *b, unsigned char c)
{
    return zsx_h_put(b, &c, 1);
}

static inline int zsx_h_var(struct zsx_h_buf *b, size_t v)
{
    do {
        unsigned char c = (unsigned char)(v & 127);
        v >>= 7;
        if (v) c |= 128;
        if (!zsx_h_byte(b, c)) return 0;
    } while (v);
    return 1;
}

static inline int zsx_h_readvar(const unsigned char **p,
                               const unsigned char *end, size_t *v)
{
    size_t result = 0;
    unsigned shift = 0, bytes = 0;
    while (*p < end) {
        unsigned char c = *(*p)++;
        size_t part = c & 127u;
        if (shift >= sizeof(size_t) * 8 || part > (SIZE_MAX >> shift))
            return 0;
        result |= part << shift;
        ++bytes;
        if (!(c & 128)) {
            if (bytes > 1 && part == 0) return 0;
            *v = result;
            return 1;
        }
        shift += 7;
    }
    return 0;
}

static inline int zsx_h_putcrc(struct zsx_h_buf *b, uint32_t v)
{
    unsigned char bytes[4];
    unsigned i;
    for (i = 0; i < 4; ++i) bytes[i] = (unsigned char)(v >> (8 * i));
    return zsx_h_put(b, bytes, 4);
}

static inline uint32_t zsx_h_getcrc(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t zsx_h_key(const unsigned char *p, size_t n)
{
    uint64_t h = UINT64_C(14695981039346656037);
    size_t i;
    for (i = 0; i < n; ++i) h = (h ^ p[i]) * UINT64_C(1099511628211);
    return h;
}

/* At most 64 probes: hostile collisions cannot cause quadratic scanning.
 * Untracked words are simply literal; the exact same search is used twice. */
static inline size_t zsx_h_find(const unsigned char *input, size_t off,
                               size_t len, const uint32_t *table,
                               const struct zsx_h_word *words)
{
    size_t slot = (size_t)zsx_h_key(input + off, len) & (ZSX_HIGH_TABLE_SIZE - 1);
    unsigned probes;
    for (probes = 0; probes < 64; ++probes) {
        uint32_t entry = table[slot];
        if (!entry || (words[entry - 1].len == len &&
            !memcmp(input + words[entry - 1].off, input + off, len)))
            return slot;
        slot = (slot + 1) & (ZSX_HIGH_TABLE_SIZE - 1);
    }
    return SIZE_MAX;
}

static inline unsigned char zsx_h_hash(const unsigned char *p, size_t n,
                                      const unsigned *rank)
{
    size_t best[3] = { SIZE_MAX, SIZE_MAX, SIZE_MAX }, i;
    unsigned k, j;
    uint32_t h = (uint32_t)n * UINT32_C(2654435761);
    for (i = 0; i < n; ++i) {
        for (k = 0; k < 3; ++k) {
            if (best[k] == SIZE_MAX || rank[p[i]] < rank[p[best[k]]]) {
                for (j = 2; j > k; --j) best[j] = best[j - 1];
                best[k] = i;
                break;
            }
        }
    }
    for (k = 0; k < 3; ++k) {
        h ^= (uint32_t)(rank[p[best[k]]] + 1) * UINT32_C(16777619);
        h = (h ^ (uint32_t)best[k]) * UINT32_C(2246822519);
        h ^= h >> 13;
    }
    return (unsigned char)((h ^ (h >> 16)) & 127);
}

static inline int zsx_h_literals(struct zsx_h_buf *b,
                                const unsigned char *p, size_t n)
{
    while (n) {
        size_t take = n > 65535 ? 65535 : n;
        if (!zsx_h_byte(b, 128) || !zsx_h_var(b, take) ||
            !zsx_h_put(b, p, take)) return 0;
        p += take; n -= take;
    }
    return 1;
}

static inline int zsx_h_encode_mode(const unsigned char *input, size_t length,
    unsigned char **out, size_t *out_len, struct zsx_high_stats *stats, int mode)
{
    struct zsx_h_buf b = { NULL, 0, 0 };
    struct zsx_h_word *words = NULL;
    uint32_t *table = NULL;
    struct zsx_high_stats s = { 0, 0, 0, 0, 0 };
    size_t freq[256] = { 0 }, count = 0, i, start, n, slot, literal;
    unsigned rank[256], a, c;
    unsigned char hashes[ZSX_HIGH_MAX_DICT];
    int slots[128];
    int ok = 0;
    if (stats) memset(stats, 0, sizeof(*stats));
    if (!out || !out_len) return 0;
    *out = NULL; *out_len = 0;
    if ((!input && length) || length > ZSX_HIGH_MAX_INPUT || !length) return 0;
    words = (struct zsx_h_word *)calloc(ZSX_HIGH_MAX_WORDS, sizeof(*words));
    table = (uint32_t *)calloc(ZSX_HIGH_TABLE_SIZE, sizeof(*table));
    if (!words || !table) goto done;
    for (i = 0; i < length; ++i) ++freq[input[i]];
    for (a = 0; a < 256; ++a) {
        rank[a] = 0;
        for (c = 0; c < 256; ++c)
            if (freq[c] > freq[a] || (freq[c] == freq[a] && c < a)) ++rank[a];
    }
    for (i = 0; i < length;) {
        if (!zsx_h_letter(input[i])) { ++i; continue; }
        start = i;
        while (i < length && zsx_h_letter(input[i])) ++i;
        n = i - start;
        if (n < 3 || n > 128) continue;
        slot = zsx_h_find(input, start, n, table, words);
        if (slot == SIZE_MAX) continue;
        if (table[slot]) ++words[table[slot] - 1].count;
        else if (count < ZSX_HIGH_MAX_WORDS) {
            words[count].off = start;
            words[count].len = (uint16_t)n;
            words[count].count = 1;
            table[slot] = (uint32_t)++count;
        }
    }
    for (i = 0; i < count; ++i) {
        if (words[i].count >= 3 && s.dictionary_entries < ZSX_HIGH_MAX_DICT) {
            size_t id = s.dictionary_entries++;
            words[i].id = (uint16_t)(id + 1);
            hashes[id] = zsx_h_hash(input + words[i].off, words[i].len, rank);
        }
    }
    if (!s.dictionary_entries) goto done;
    if (!zsx_h_put(&b, "ZHP1", 4) || !zsx_h_byte(&b, (unsigned char)mode) ||
        !zsx_h_var(&b, length) || !zsx_h_putcrc(&b, zsx_h_crc(input, length)) ||
        !zsx_h_var(&b, s.dictionary_entries)) goto done;
    for (i = 0; i < count; ++i) if (words[i].id) {
        if (!zsx_h_var(&b, words[i].len) ||
            !zsx_h_byte(&b, hashes[words[i].id - 1]) ||
            !zsx_h_put(&b, input + words[i].off, words[i].len)) goto done;
    }
    for (i = 0; i < 128; ++i) slots[i] = -1;
    literal = 0;
    for (i = 0; i < length;) {
        size_t id;
        unsigned char hash;
        if (!zsx_h_letter(input[i])) { ++i; continue; }
        start = i;
        while (i < length && zsx_h_letter(input[i])) ++i;
        n = i - start;
        if (n < 3 || n > 128) continue;
        slot = zsx_h_find(input, start, n, table, words);
        if (slot == SIZE_MAX || !table[slot] || !words[table[slot] - 1].id)
            continue;
        id = words[table[slot] - 1].id - 1;
        hash = hashes[id];
        if (!zsx_h_literals(&b, input + literal, start - literal)) goto done;
        if (mode) {
            if (!zsx_h_byte(&b, 130) || !zsx_h_var(&b, id)) goto done;
        } else if (slots[hash] == (int)id) {
            if (!zsx_h_byte(&b, hash)) goto done;
            ++s.hash_hits;
        } else {
            if (!zsx_h_byte(&b, 129) || !zsx_h_var(&b, id)) goto done;
            ++s.definitions;
            if (slots[hash] >= 0) ++s.collisions;
            slots[hash] = (int)id;
        }
        ++s.words;
        literal = i;
    }
    if (!zsx_h_literals(&b, input + literal, length - literal) ||
        !zsx_h_putcrc(&b, zsx_h_crc(b.p, b.n))) goto done;
    *out = b.p; *out_len = b.n; b.p = NULL;
    if (stats) *stats = s;
    ok = 1;
done:
    free(b.p); free(words); free(table);
    return ok;
}

static inline int zsx_high_encode(const unsigned char *input, size_t length,
    unsigned char **out, size_t *out_len, struct zsx_high_stats *stats)
{
    return zsx_h_encode_mode(input, length, out, out_len, stats, 0);
}

static inline int zsx_high_encode_indices(const unsigned char *input, size_t length,
    unsigned char **out, size_t *out_len, struct zsx_high_stats *stats)
{
    return zsx_h_encode_mode(input, length, out, out_len, stats, 1);
}

static inline int zsx_high_decode(const unsigned char *input, size_t length,
    unsigned char **out, size_t *out_len, size_t max_output)
{
    const unsigned char *p, *end;
    unsigned char *result = NULL;
    struct zsx_h_entry dict[ZSX_HIGH_MAX_DICT];
    int slots[128];
    size_t total, count, i, j, n, used = 0, id;
    uint32_t crc;
    unsigned mode, token;
    if (!out || !out_len) return 0;
    *out = NULL; *out_len = 0;
    if (!input || length < 15 || memcmp(input, "ZHP1", 4)) return 0;
    if (zsx_h_crc(input, length - 4) != zsx_h_getcrc(input + length - 4))
        return 0;
    p = input + 4; end = input + length - 4;
    mode = *p++;
    if (mode > 1 || !zsx_h_readvar(&p, end, &total) || !total ||
        total > max_output || total > ZSX_HIGH_MAX_INPUT || end - p < 4)
        return 0;
    crc = zsx_h_getcrc(p); p += 4;
    if (!zsx_h_readvar(&p, end, &count) || !count || count > ZSX_HIGH_MAX_DICT)
        return 0;
    for (i = 0; i < count; ++i) {
        if (!zsx_h_readvar(&p, end, &n) || n < 3 || n > 128 || p == end)
            return 0;
        dict[i].hash = *p++;
        if (dict[i].hash > 127 || n > (size_t)(end - p)) return 0;
        for (j = 0; j < n; ++j) if (!zsx_h_letter(p[j])) return 0;
        dict[i].p = p; dict[i].len = n; p += n;
    }
    result = (unsigned char *)malloc(total);
    if (!result) return 0;
    for (i = 0; i < 128; ++i) slots[i] = -1;
    while (p < end) {
        const unsigned char *source;
        token = *p++;
        if (token == 128) {
            if (!zsx_h_readvar(&p, end, &n) || !n || n > 65535 ||
                n > (size_t)(end - p)) goto invalid;
            source = p; p += n;
        } else {
            if (token < 128) {
                if (mode || slots[token] < 0) goto invalid;
                id = (size_t)slots[token];
            } else if ((token == 129 && !mode) || (token == 130 && mode)) {
                if (!zsx_h_readvar(&p, end, &id) || id >= count) goto invalid;
                if (!mode) {
                    unsigned hash = dict[id].hash;
                    if (slots[hash] == (int)id) goto invalid;
                    slots[hash] = (int)id;
                }
            } else goto invalid;
            source = dict[id].p; n = dict[id].len;
        }
        if (n > total - used) goto invalid;
        memcpy(result + used, source, n); used += n;
    }
    if (used != total || zsx_h_crc(result, total) != crc) goto invalid;
    *out = result; *out_len = total;
    return 1;
invalid:
    free(result);
    return 0;
}

#endif