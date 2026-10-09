/* Compare complete dictionary transforms after the SAME baseline coder.
 * Build: cc -O2 -std=c11 experimental/high_dictionary_bench.c -o /tmp/high-bench
 * Usage: /tmp/high-bench FILE (one chunk, at most 12 MiB).
 */
#define main zsx_cli_main
#include "../zsx.c"
#undef main

static size_t baseline_size(const unsigned char *data, size_t length) {
	if (length > zsx_chunk) return 0;
	byte *buffer = zsx_new(byte, (size_t)zsx_chunk + header_sizes + 1);
	memcpy(buffer, data, length);
	zsx_len(buffer) = length;
	zsx_encode(buffer);
	size_t result = zsx_len(buffer);
	zsx_del(buffer);
	return result;
}

int main(int argc, char **argv) {
	if (argc != 2) return 2;
	FILE *file = fopen(argv[1], "rb");
	if (!file) return 2;
	unsigned char *data = (unsigned char *)malloc((size_t)zsx_chunk + 1);
	if (!data) { fclose(file); return 2; }
	size_t length = fread(data, 1, (size_t)zsx_chunk + 1, file);
	int failed = ferror(file) || length > zsx_chunk;
	fclose(file);
	if (failed) { free(data); return 2; }
	size_t base = baseline_size(data, length);
	printf("kind,source_bytes,transform_bytes,complete_archive_bytes,dictionary,hits,definitions,collisions\n");
	printf("baseline,%zu,%zu,%zu,0,0,0,0\n", length, length, base + 20);
	for (int indices = 0; indices < 2; indices++) {
		unsigned char *encoded = NULL;
		size_t encoded_length = 0;
		struct zsx_high_stats stats = {0};
		int ok = indices ?
			zsx_high_encode_indices(data, length, &encoded, &encoded_length, &stats) :
			zsx_high_encode(data, length, &encoded, &encoded_length, &stats);
		if (ok) {
			unsigned char *decoded = NULL;
			size_t decoded_length = 0;
			if (!zsx_high_decode(encoded, encoded_length, &decoded, &decoded_length, length) ||
				decoded_length != length || memcmp(data, decoded, length)) {
				fprintf(stderr, "dictionary transform round trip failed\n");
				free(decoded); free(encoded); free(data);
				return 1;
			}
			free(decoded);
			size_t packed = baseline_size(encoded, encoded_length);
			printf("%s,%zu,%zu,%zu,%zu,%zu,%zu,%zu\n",
				indices ? "indices" : "hash", length, encoded_length,
				packed ? packed + 20 : 0, stats.dictionary_entries,
				stats.hash_hits, stats.definitions, stats.collisions);
		}
		free(encoded);
	}
	free(data);
	return 0;
}