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
	x[0] = item_size;g
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

writer_t *flash(wrinter_t *w) {
	writer_t *writer = _new(writer_t, 1);
	writer->power = w->power;
	writer->bits = w->bits;
	writer->start = w->start;
	writer->data = w->data;
	return writer;
}

/*----- sx related -----*/

int lf(unsigned int n) {
	int m = 1;
	while ((unsigned int)(1 << m) < n)
		m++;
	return m;
}


#define sx_chunk_bits 25
#define sx_chunk 1 << sx_chunk_bits
#define sx_bits 8
#define sx_values 256
#define sx_win 16*1024*1024
#define sx_max 256*1024
#define sx_items 3

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
		destination->cnt = 0;
		root->data[x[sx_items - len]] = destination;
	}
	if (len == 1) {
		destination->cnt = data;
	}
	else {
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


void zsx_del(zsx_node *root) {
	if (root->cnt == 0)
		for (int i = 0; i < sx_values; i++)
			if (root->data[i])
				zsx_del(root->data[i]);
	_del(root)
}

int zsx[sx_win];
int cz[sx_values];
int cs[sx_values];
int cx[sx_values];

void zsx_dump(writer_t *w, zsx_node *root, int last)
{
	int zsx_counter = 0;
	for (int s = 0; s < sx_values; s++)
		write_bit(w, cz[s]), write_bit(w, cs[s]), write_bit(w, cx[s]);

	for (int z = 0; z < sx_values; z++)if(cz[z])
	{
		zsx_node *znode = root->data[z];		
		if (znode)
		{			
			for (int s = 0; s < sx_values; s++)if(cs[s])
			{
				zsx_node *snode = znode->data[s];				
				if (snode)
				{	
					
					for (int x = 0; x < sx_values; x++)if(cx[x])
					{
						zsx_node *xnode = snode->data[x];				
						if (xnode)
						{							
							zsx[xnode->cnt - 1] = zsx_counter++;						
						}
					}
				}
			}
		}
	}

	for (int s = 0; s < sx_values; s++)
		cz[s] = 0, cs[s] = 0, cx[s] = 0;
	
	for (int s = 0; s < last; s++)
	{
		write_value(w, zsx[s], lf(zsx_counter));
	}
}


char *bytes_buffer;
char *result_buffer;

#define ZSX

/****** zsx ******/
byte *zsx_encode(byte *data, int len) {
	int z, s, x;
	byte *result;
	writer_t *bytes = new_writer(bytes_buffer);
	reader_t *reader = new_reader(data);
	write_value(bytes, len, 32);

	printf(".");

	zsx_node *root = _new(zsx_node, 1);
	int last = 0;
	int sx[sx_values];
	for (s = 0; s < sx_values; s++)
		cz[s] = 0, cs[s] = 0, cx[x] = 0;

	int lx = 0;
	int ls = 0;
	int lz = 0;

	writer_t *save = flash(bytes);
	write_value(bytes, 0, sx_chunk_bits);

	int count = 0;

	while (reader->start <= len) {

		z = read_value(reader, sx_bits);
		s = read_value(reader, sx_bits);
		x = read_value(reader, sx_bits);
		
		count++;
		
		int list[] = { z,s,x };
		int index = zsx_get(root, list, sx_items);
		write_bit(bytes, index ? 1 : 0);
		if (index)
			write_value(bytes, index - 1, lf(last));
		else
		{
			zsx_set(root, list, sx_items, ++last);
			if (cz[z] == 0)lz++;
			cz[z] = 1;
			if (cs[s] == 0)ls++;
			cs[s] = 1;
			if (cx[x] == 0)lx++;
			cx[x] = 1;
		}

		if ((lz*ls*lx)> sx_max)
		{
			zsx_dump(bytes, root, last);
			zsx_del(root);
			root = _new(zsx_node, 1);
			last = 0;
			lz = 0;
			ls = 0;
			lx = 0;
			
			write_value(save, count, sx_chunk_bits);
			_del(save);
			save = flash(bytes);
			write_value(bytes, 0, sx_chunk_bits);

			count = 0;
		}
	}

	zsx_dump(bytes, root, last);
	zsx_del(root);
	write_value(save, count, sx_chunk_bits);
	_del(save);

	flushWrite(bytes);
	*((size_t *)result_buffer - 1) = bytes->start;
	result = result_buffer;

	memcpy(result, bytes->data, bytes->start);
	
	_del(bytes);

	return result;
}

byte *zsx_decode(byte *data, int len) {
	reader_t *bytes = new_reader(data);
	unsigned int z, s, x, size = read_value(bytes, 32);

	puts("decoding...");

	byte *result = _new(byte, size+12);
	*((size_t *)result_buffer - 1) = size;
	writer_t *w = new_writer(result);
	
	int **dico = _new(int *, sx_win);
	int **diko = _new(int *, 64*1024);
	int *codes = _new(int, len);
	int *rest = _new(int, sx_win);
	int items = size;
	int decoded = 0;
	int count = 0;
	int index = 0;
	int last = 0;

	for(s=0;s<sx_win;s++)
	{
		if(s<64*1024)
			diko[s] = _new(int, 4);
		dico[s]= _new(int, 4);
	}
	while (decoded < items) {
		
		if(read_bit(bytes))
		{
			index = read_value(bytes, lf(last));
			codes[count++] = index;
		}else
		{
			x = read_value(bytes, sx_bits);
			rest[last] = x;
			codes[count++]=last++;
		}
		if(last == sx_win || (count*3+decoded) >= items)
		{
			int max = 0;
			for (z = 0; z < sx_values; z++)	
				for(s = 0; s < sx_values; s++)
					if(read_bit(bytes))
					{
						diko[max][0] = z;
						diko[max][1] = s;
						max++;
					}
			for(index=0;index<last;index++)
			{
				z = read_value(bytes, lf(max));
				dico[index][0] = diko[z][0]; //z
				dico[index][1] = diko[z][1]; //s
				dico[index][2] = rest[index];//x
			}
			
			for(s=0;s<count;s++)
			{
				result[decoded++] = dico[codes[s]][0];
				result[decoded++] = dico[codes[s]][1];
				result[decoded++] = dico[codes[s]][2];
			}
			
			
			last = 0;
			count = 0;
		}

	}
	
	
	for(s=0;s<last;s++)
	{
		if(s<64*1024)
			_del(diko[s]);
		_del(dico[s]);
	}
	_del(dico);
	_del(diko);
	
	return result;
}

int test() {
	int i, fd = open("enwik9.1024", O_RDONLY);
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

	FILE *f = fopen("enwik9.1024.dec", "wb");

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
