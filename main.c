#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

/* -------- aux stuff ---------- */

typedef uint8_t byte;

void *mem_alloc(size_t item_size, size_t n_item) {
	size_t *x = (size_t *)calloc(1, sizeof(size_t) * 2 + n_item * item_size);
	x[0] = item_size;
	x[1] = n_item;
	return x + 2;
}

void *mem_extend(void *m, size_t new_n) {
	size_t *x = (size_t *)m - 2;
	x = (size_t *)realloc(x, sizeof(size_t) * 2 + *x * new_n);
	if (new_n > x[1])
		memset((char *)(x + 2) + x[0] * x[1], 0, x[0] * (new_n - x[1]));
	x[1] = new_n;
	return x + 2;
}

void _clear(void *m) {
	size_t *x = (size_t *)m - 2;
	memset(m, 0, x[0] * x[1]);
}

#define _new(type, n) mem_alloc(sizeof(type), n)
#define _del(m) free((size_t *)(m)-2);
#define _len(m) *((size_t *)m - 1)
#define _setsize(m, n) m = (byte *)mem_extend(m, n)
#define _extend(m) m = (byte *)mem_extend(m, _len(m) * 2);

/*--------- bit reader/writer---------*/

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} writer_t;

void write_bit(writer_t *p, int v) {
	p->bits += (v & 1) << (8 - (++p->power));
	if (p->power >= 8) {
		p->data[p->start++] = p->bits;
		p->power = 0;
		p->bits = 0;
		if (p->start > _len(p->data) - 4)
			_extend(p->data)
	}
}

void write_value(writer_t *p, unsigned long v, int n) {
	while (n > 0) {
		n--;
		write_bit(p, (v >> n) & 1);
	}
}

void flushWrite(writer_t *p) {
	while (p->power > 0)
		write_bit(p, 0);
}

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} reader_t;

int read_bit(reader_t *r) {
	int v = (r->bits >> (8 - (++r->power)) & 1);
	if (r->power >= 8) {
		r->power = 0;
		r->bits = r->data[r->start++];
	}
	return v;
}

int read_value(reader_t *r, int n) {
	int v = 0;
	while (n > 0) {
		n--;
		v = v << 1;
		v += read_bit(r);
	}
	return v;
}

writer_t *new_writer(byte *out) {
	writer_t *r = _new(writer_t, 1);
	r->power = 0;
	r->bits = 0;
	r->start = 0;
	r->data = (char *)out;
	return r;
}

reader_t *new_reader(byte *in) {
	reader_t *r = _new(reader_t, 1);
	r->power = 0;
	r->bits = in[0];
	r->start = 1;
	r->data = (char *)in;
	return r;
}

/*----- sx related -----*/

int lf(unsigned int n) {
	int m = 1;
	while ((unsigned int)(1 << m) < n)
		m++;
	return m;
}

#define sx_chunk_bits 28
#define sx_chunk 1 << sx_chunk_bits
#define sx_bits 8
#define sx_half 4
#define sx_rest 16
#define sx_values 4096
#define sx_win 4 * 1024
#define sx_items 2

typedef struct {
	void *data[sx_values];
	int cnt;
} zsx_node;

void zsx_set(zsx_node *root, int *x, int len, int data) {
	zsx_node *destination = (zsx_node *)(root->data[x[sx_items - len]]);
	if (destination == 0) {
		destination = _new(zsx_node, 1);
		for (int i = 0; i < sx_values; i++)
			destination->data[i] = 0;
		root->data[x[sx_items - len]] = destination;
	}
	if (len == 1) {
		destination->cnt = data;
	} else {
		zsx_set(destination, x, len - 1, data);
	}
}

int zsx_get(zsx_node *root, int *x, int len) {
	zsx_node *destination = (zsx_node *)(root->data[x[sx_items - len]]);
	if (destination == 0)
		return 0;
	if (len == 1)
		return destination->cnt;
	else
		return zsx_get(destination, x, len - 1);
}

void zsx_dump(writer_t *w, zsx_node *root, int *dico) {
	int zsx_counter = 0;
	for (int z = 0; z < sx_values; z++) {
		zsx_node *initial = root->data[z];
    write_bit(w, initial?1:0);
    if (initial)
		{
			for (int s = 0; s < sx_rest; s++) {
				zsx_node *found = initial->data[s];
				
				if (found)
				{
					write_value(w, s, 4);
					dico[found->cnt] = zsx_counter++;
				}
			}
		}
	}
}

void zsx_del(zsx_node *root) {
	for (int i = 0; i < sx_values; i++)
		if (root->data[i])
			zsx_del(root->data[i]);
	_del(root)
}

int sorted[sx_win * 2];

char *bytes_buffer;
char *result_buffer;

int list[16];

#define ZSX

/****** zsx ******/
byte *zsx_encode(byte *data, int len) {
	int z, s, x, k, v, n;
	byte *result;
	writer_t *bytes = new_writer(bytes_buffer);
	reader_t *reader = new_reader(data);
	write_value(bytes, len, 32);

	printf(".");

	zsx_node *root = _new(zsx_node, 1);

	int last = 0;
	int items = 0;
	int *codes = _new(int, len);
	int half;
	int count = 0;

	while (reader->start <= len) {
		z = 0;
		s = read_value(reader, sx_bits);
		x = read_value(reader, sx_bits);

		list[z++] = (s << sx_half) ^ x;
		list[z++] = x >> sx_half;
		
		x = zsx_get(root, list, z);
		write_bit(bytes, x?1:0);
		if (x) {
			codes[count++] = x;
		} else {
			zsx_set(root, list, z, ++last);
		}

		if (last >= sx_win) {
			zsx_dump(bytes, root, sorted);
			zsx_del(root);
			write_value(bytes, count, sx_chunk_bits);
			for (s = 0; s < count; s++)
				write_value(bytes, sorted[codes[s]], lf(last));
			root = _new(zsx_node, 1);
			last = 0;
			count = 0;
		}
	}
	zsx_dump(bytes, root, sorted);
	zsx_del(root);
	write_value(bytes, count, sx_chunk_bits);
	for (s = 0; s < count; s++)
		write_value(bytes, sorted[codes[s]], lf(last));
	_del(codes) 
	

		flushWrite(bytes);
	*((size_t *)result_buffer - 1) = bytes->start;
	result = result_buffer;

	memcpy(result, bytes->data, bytes->start);

	return result;
}

byte *zsx_decode(byte *data, int len) {
	reader_t *bytes = new_reader(data);
	unsigned int z, s, x, k, size = read_value(bytes, 32);

	puts("decoding...");

	byte *result = _new(byte, size);

	int **dico = _new(int *, size);
	int decoded = 0;
	int items = size;
	int max = 0;
	int last = 0;
	int count = 0;
	int half = 0;

	while (decoded < items) {
		max = read_value(bytes, lf(sx_win)) + 1;

		last = 0;
		for (z = 0; z < sx_values; z++)
			for (s = 0; s < sx_values; s++)
				if (read_bit(bytes)) {
					dico[last] = _new(int, 4);
					dico[last][0] = z;
					dico[last][1] = s;
					last++;
				}

		count = read_value(bytes, sx_chunk_bits);
		for (z = 0; z < count; z++) {
			s = read_value(bytes, lf(max));
			result[decoded++] = dico[s][0];
			result[decoded++] = dico[s][1];
		}

		for (z = 0; z < last; z++)
			_del(dico[z])
	}

	_del(dico);

	return result;
}

int test() {
	int i, fd = open("enwik9", O_RDONLY);
	if (fd == -1) {
		printf("Can't read file!\n");
		return 0;
	};

	struct stat st;
	fstat(fd, &st);

	int encoded = 0;
	byte *in = (byte *)_new(char, sx_chunk);
	bytes_buffer = _new(char, sx_chunk);
	result_buffer = _new(char, sx_chunk);

	FILE *f = fopen("enwik9.zsx", "wb");

	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};

	printf("encoding...\n");

	while (encoded < st.st_size) {
		int bytes_read = read(fd, in, sx_chunk);
		encoded += bytes_read;

		int prev_len = bytes_read;
		byte *data = zsx_encode(in, bytes_read);

#ifdef ZSX

		size_t len_zsx = _len(data);

		while (len_zsx < (prev_len)) {
			prev_len = len_zsx;
			data = zsx_encode(data, len_zsx);
			len_zsx = _len(data);
		}

		printf("final size: %ld.\n", len_zsx);

		fwrite(&len_zsx, sizeof(size_t), 1, f);
		fwrite(data, len_zsx, 1, f);

		printf("%.2f%%\n", (double)encoded / (double)st.st_size * 100.0);

#else

		byte *decoded = zsx_decode(data, _len(data));
		fwrite(decoded, _len(decoded), 1, f);
		puts(decoded);
		printf("%.2f%%.\n", (double)encoded / (double)st.st_size * 100.0);

#endif
	}
	fclose(f);
	close(fd);

	return 1;
}

int main(int argc, char **argv) {
	test();
	return 0;
}
