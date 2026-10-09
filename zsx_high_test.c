/* Standalone: cc -std=c11 -Wall -Wextra -Werror -O2 zsx_high_test.c -o /tmp/zsx-high-test */
#include "zsx_high.h"
#include <assert.h>
#include <stdio.h>

static void roundtrip(const unsigned char *text, size_t len, int collisions)
{
    unsigned char *coded, *coded2, *plain;
    size_t size, size2, decoded, cut;
    struct zsx_high_stats stats;
    int mode;
    for (mode = 0; mode < 2; ++mode) {
        assert(zsx_h_encode_mode(text, len, &coded, &size, &stats, mode));
        assert(zsx_h_encode_mode(text, len, &coded2, &size2, NULL, mode));
        assert(size == size2 && !memcmp(coded, coded2, size));
        free(coded2);
        assert(zsx_high_decode(coded, size, &plain, &decoded, len));
        assert(decoded == len && !memcmp(plain, text, len));
        free(plain);
        assert(!zsx_high_decode(coded, size, &plain, &decoded, len - 1));
        assert(plain == NULL && decoded == 0);
        if (!mode && collisions) assert(stats.collisions > 0);
        for (cut = 0; cut < size; ++cut)
            assert(!zsx_high_decode(coded, cut, &plain, &decoded, len));
        for (cut = 0; cut < size; ++cut) {
            coded[cut] ^= 1;
            assert(!zsx_high_decode(coded, size, &plain, &decoded, len));
            coded[cut] ^= 1;
        }
        printf("%s: input=%zu transform=%zu words=%zu entries=%zu hits=%zu "
               "definitions=%zu collisions=%zu\n", mode ? "indices" : "hash",
               len, size, stats.words, stats.dictionary_entries, stats.hash_hits,
               stats.definitions, stats.collisions);
        free(coded);
    }
}

/* Deliberately construct valid-checksum containers to exercise token parsing. */
static void token_case(const unsigned char *tokens, size_t len, int valid)
{
    struct zsx_h_buf b = { NULL, 0, 0 };
    unsigned char *plain;
    size_t n;
    assert(zsx_h_put(&b, "ZHP1\0", 5));
    assert(zsx_h_var(&b, 3));
    assert(zsx_h_putcrc(&b, zsx_h_crc((const unsigned char *)"cat", 3)));
    assert(zsx_h_var(&b, 1));
    assert(zsx_h_var(&b, 3));
    assert(zsx_h_byte(&b, 7));
    assert(zsx_h_put(&b, "cat", 3));
    assert(zsx_h_put(&b, tokens, len));
    assert(zsx_h_putcrc(&b, zsx_h_crc(b.p, b.n)));
    assert(zsx_high_decode(b.p, b.n, &plain, &n, 3) == valid);
    if (valid) { assert(n == 3 && !memcmp(plain, "cat", 3)); free(plain); }
    else assert(plain == NULL && n == 0);
    free(b.p);
}

int main(void)
{
    static const unsigned char unicode[] =
        "Hello \xc3\xa9 world \xf0\x9f\x8c\x8d Hello world Hello world\0"
        "MixedCASE MixedCASE MixedCASE abc abc abc";
    unsigned char forced[4096], longword[600], *out;
    size_t n = 0, out_len, i;
    unsigned pass, word;
    struct zsx_high_stats stats;
    const unsigned char define[] = {129, 0};
    const unsigned char undefined[] = {7};
    const unsigned char badindex[] = {129, 1};
    const unsigned char noncanonical[] = {129, 128, 0};
    const unsigned char zero_literal[] = {128, 0};
    const unsigned char wrongmode[] = {130, 0};
    const unsigned char extra[] = {129, 0, 7};
    roundtrip(unicode, sizeof(unicode) - 1, 0);
    /* Pigeonhole: 129 distinct words in 128 slots guarantee replacements. */
    for (pass = 0; pass < 4; ++pass)
        for (word = 0; word < 129; ++word) {
            forced[n++] = 'w';
            forced[n++] = (unsigned char)('a' + word / 26);
            forced[n++] = (unsigned char)('a' + word % 26);
            forced[n++] = ' ';
        }
    roundtrip(forced, n, 1);
    memset(longword, 'a', sizeof(longword));
    memcpy(longword, "cat cat cat ", 12);
    longword[300] = 0xff;
    roundtrip(longword, sizeof(longword), 0);
    assert(!zsx_high_encode(NULL, 0, &out, &out_len, &stats));
    assert(!out && !out_len && !stats.words);
    assert(!zsx_high_encode((const unsigned char *)"one two", 7,
                            &out, &out_len, &stats));
    assert(!zsx_high_encode_indices(NULL, ZSX_HIGH_MAX_INPUT + 1,
                                    &out, &out_len, &stats));
    token_case(define, sizeof(define), 1);
    token_case(undefined, sizeof(undefined), 0);
    token_case(badindex, sizeof(badindex), 0);
    token_case(noncanonical, sizeof(noncanonical), 0);
    token_case(zero_literal, sizeof(zero_literal), 0);
    token_case(wrongmode, sizeof(wrongmode), 0);
    token_case(extra, sizeof(extra), 0);
    /* Repeated per-byte corruption with repaired container CRC reaches deeper
     * decoder paths; any accepted mutation must still satisfy original CRC. */
    assert(zsx_high_encode(unicode, sizeof(unicode) - 1, &out, &out_len, NULL));
    for (i = 4; i < out_len - 4; ++i) {
        unsigned char *plain;
        size_t decoded;
        uint32_t crc;
        unsigned j;
        out[i] ^= 0x80;
        crc = zsx_h_crc(out, out_len - 4);
        for (j = 0; j < 4; ++j) out[out_len - 4 + j] = (unsigned char)(crc >> (j * 8));
        if (zsx_high_decode(out, out_len, &plain, &decoded, sizeof(unicode))) {
            assert(decoded == sizeof(unicode) - 1);
            assert(!memcmp(plain, unicode, decoded));
            free(plain);
        }
        out[i] ^= 0x80;
    }
    free(out);
    puts("zsx_high tests passed");
    return 0;
}