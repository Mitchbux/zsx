/* compile : 
 tcc fastwin32.c -lcabinet -o shrink.exe
*/
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>
#include <compressapi.h>


/* -------- aux stuff ---------- */

typedef uint8_t byte;

void *zsx_alloc(size_t item_size, size_t n_item) {
	size_t *x = (size_t *)calloc(1, sizeof(size_t) * 2 + n_item * item_size);
	x[0] = item_size;
	x[1] = n_item;
	return x + 2;
}

void *zsx_extend(void *m, size_t new_n) {
	size_t *x = (size_t *)m - 2;
	x = (size_t *)realloc(x, sizeof(size_t) * 2 + *x * new_n);
	if (new_n > x[1])
		memset((char *)(x + 2) + x[0] * x[1], 0, x[0] * (new_n - x[1]));
	x[1] = new_n;
	return x + 2;
}

void zsx_clear(void *m) {
	size_t *x = (size_t *)m - 2;
	memset(m, 0, x[0] * x[1]);
}

#define zsx_new(type, n) zsx_alloc(sizeof(type), n)
#define zsx_del(m) free((size_t *)(m)-2);
#define zsx_len(m) *((size_t *)m - 1)
#define zsx_setsize(m, n) m = (byte *)zsx_extend(m, n)
#define zsx_extend(m) m = (byte *)zsx_extend(m, _len(m) * 2);

/*--------- bit reader/writer---------*/

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} zsx_writer_t;

void zsx_write_bit(zsx_writer_t *p, int v) {
	p->bits += (v & 1) << (8 - (++p->power));
	if (p->power >= 8) {
		p->data[p->start++] = p->bits;
		p->power = 0;
		p->bits = 0;
	}
}

void zsx_write_value(zsx_writer_t *p, unsigned long v, int n) {
	while (n > 0) {
		n--;
		zsx_write_bit(p, (v >> n) & 1);
	}
}

void zsx_flushWrite(zsx_writer_t *p) {
	while (p->power > 0)
		zsx_write_bit(p, 0);
}

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} zsx_reader_t;

int zsx_read_bit(zsx_reader_t *r) {
	int v = (r->bits >> (8 - (++r->power)) & 1);
	if (r->power >= 8) {
		r->power = 0;
		r->bits = r->data[r->start++];
	}
	return v;
}

unsigned int zsx_read_value(zsx_reader_t *r, int n) {
	unsigned int v = 0;
	while (n > 0) {
		n--;
		v = v << 1;
		v += zsx_read_bit(r);
	}
	return v;
}

zsx_writer_t *zsx_writer(byte *out) {
	zsx_writer_t *r = zsx_new(zsx_writer_t, 1);
	r->power = 0;
	r->bits = 0;
	r->start = 0;
	r->data = (char *)out;
	return r;
}

zsx_reader_t *zsx_reader(byte *in) {
	zsx_reader_t *r = zsx_new(zsx_reader_t, 1);
	r->power = 0;
	r->bits = in[0];
	r->start = 1;
	r->data = (char *)in;
	return r;
}

int zsx_lf(unsigned long n) {
	int m = 1;
	while ((unsigned long)(1 << m) < n)
		m++;
	return m;
}

void merge (zsx_writer_t *w, int *a, int n, int m) {
    int i, j, k;
    int *x = malloc(n * sizeof (int));
    for (i = 0, j = m, k = 0; k < n; k++) {
		if(j<n&&i<m&&w)zsx_write_bit(w, a[j]<a[i]?1:0);
        x[k] = j == n      ? a[i++]
             : i == m      ? a[j++]
             : a[j] < a[i] ? a[j++]
             :               a[i++];
    }
    for (i = 0; i < n; i++) {
        a[i] = x[i];
    }
    free(x);
}

void merge_sort (zsx_writer_t *w, int *a, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    merge_sort(w, a, m);
    merge_sort(w, a + m, n - m);
    merge(w, a, n, m);
}

void merge_read (zsx_reader_t *r, int *a, int n, int m) {
    int i, j, k;
    int *x = malloc(n * sizeof (int));
    for (i = 0, j = m, k = 0; k < n; k++) {
        x[k] = j == n      ? a[i++]
             : i == m      ? a[j++]
             : zsx_read_bit(r) ? a[j++]
             :               a[i++];
    }
    for (i = 0; i < n; i++) {
        a[i] = x[i];
    }
    free(x);
}

void merge_sort_read (zsx_reader_t *r, int *a, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    merge_sort_read(r, a, m);
    merge_sort_read(r, a + m, n - m);
    merge_read(r, a, n, m);
}

#define zsx_chunk_bits 30
#define zsx_chunk 1 << zsx_chunk_bits

#define zsx_bits 8
#define zsx_values 256
#define zsx_items 2
#define zsx_top 4096

int zsx_NextIndex;
int zsx_Next[1<<(zsx_bits*zsx_items)];
int zsx_Unknown[1<<(zsx_bits*zsx_items)];
int zsx_Initial[1<<(zsx_bits*zsx_items)][zsx_items];

int zsx_Indexes[1<<(zsx_bits*zsx_items)];
int zsx_BadIndex[1<<(zsx_bits*zsx_items)];
int zsx_BadChunk[1<<(zsx_bits*zsx_items)];
int zsx_BadSize[1<<(zsx_bits*zsx_items)];

char *zsx_bytes_buffer;
char *zsx_result_buffer;


#define ZSX

/****** zsx ******/
byte *zsx_encode(byte *data) {
	int len = zsx_len(data);
	int z, s, x;
	byte *result;
	zsx_writer_t *bytes = zsx_writer(zsx_bytes_buffer);
	zsx_reader_t *reader = zsx_reader(data);
	zsx_write_value(bytes, len, 32);

	printf(".");

	zsx_NextIndex = 0;
	int last = 0;
	int chunk = 0;
	for(s=0;s<1<<(zsx_bits*zsx_items);s++)
		zsx_BadIndex[s]=zsx_Indexes[s]=0;
	int max = 0;
	int total = 0;
	while (reader->start <= len) {
		int s = zsx_read_value(reader, zsx_bits);
		int x = zsx_read_value(reader, zsx_bits);

		int both = (s<<zsx_bits)^x;

		int index = zsx_Indexes[both];
		zsx_write_bit(bytes, index ? 1 : 0);
		if (index)
		{
			zsx_write_value(bytes, index - 1, zsx_lf(last));
			if (zsx_Unknown[index])
			{
				zsx_write_value(bytes, s, zsx_bits);
				zsx_write_value(bytes, x, zsx_bits);
				zsx_Unknown[index] = 0;
			}

		}
		else if (zsx_BadIndex[both])
		{
			zsx_write_bit(bytes, 1);
			zsx_write_value(bytes, zsx_BadChunk[both]- 1, zsx_lf(chunk));
			zsx_write_value(bytes, zsx_BadIndex[both]- 1, zsx_lf(zsx_BadSize[zsx_BadChunk[both]]));
		}else
		{
			zsx_write_bit(bytes, 0);
			int n = last++;
			zsx_Initial[n][0] = s;
			zsx_Initial[n][1] = x;
			zsx_Indexes[both] = n + 1;
			zsx_Unknown[n+1] = 1;
		}
		if(last==zsx_top)
		{
			for(x=0,s=0;s<last;s++)
			{
				if(zsx_Unknown[s+1])
				{
					int both = (zsx_Initial[s][0]<<zsx_bits)^zsx_Initial[s][1];
					zsx_BadIndex[both]=++x;
					zsx_BadChunk[both]=chunk;
				}
				zsx_Indexes[s]=0;
				zsx_Unknown[s]=0;
			}
			zsx_BadSize[chunk++]=x;
			last = 0;
			total += x;
		}
		if(total > 32*1024)
		{
			for(s=0;s<1<<(zsx_bits*zsx_items);s++)
			{
				zsx_write_bit(bytes, zsx_BadIndex[s]?1:0);
				if(zsx_BadIndex[s])
				{
					zsx_write_value(bytes, zsx_BadChunk[s]- 1, zsx_lf(chunk));
					zsx_write_value(bytes, zsx_BadIndex[s] - 1, zsx_lf(zsx_BadSize[zsx_BadChunk[s]]));
				}
				zsx_BadIndex[s]=0;
			}
			chunk = 0;
			total = 0;
		}
		
	}
		
	for (s = 0; s < last; s++)
	{
		if (zsx_Unknown[s + 1])
		{
			zsx_write_value(bytes, zsx_Initial[s][0], zsx_bits);
			zsx_write_value(bytes, zsx_Initial[s][1], zsx_bits);
		}
	}
	
	for(s=0;s<1<<(zsx_bits*zsx_items);s++)
	{
		zsx_write_bit(bytes, zsx_BadIndex[s]?1:0);
		if(zsx_BadIndex[s])
		{
			zsx_write_value(bytes, zsx_BadChunk[s]- 1, zsx_lf(chunk));
			zsx_write_value(bytes, zsx_BadIndex[s] - 1, zsx_lf(zsx_BadSize[zsx_BadChunk[s]]));
		}
	}

	//printf("%d ", bytes->start);
	zsx_flushWrite(bytes);
	*((size_t *)zsx_result_buffer - 1) = bytes->start;
	result = zsx_result_buffer;

	memcpy(result, bytes->data, bytes->start);

	return result;
}

byte *zsx_decode(byte *data, int len) {
	zsx_reader_t *bytes = zsx_reader(data);
	unsigned int z, s, x, size = zsx_read_value(bytes, 32);

	puts("decoding...");
	byte * result = zsx_bytes_buffer;
	zsx_writer_t *w = zsx_writer(result);

	int **dico = zsx_new(int *, 4096);
	int **codi = zsx_new(int *, 4096);
	int *codes = zsx_new(int, len);
	int items = size*2;
	int decoded = 0;
	int count = 0;
	int index = 0;
	int last = 0;
	for (s = 0; s < 4096; s++)
	{
		dico[s] = zsx_new(int, 4);
		codi[s] = zsx_new(int, 4);
	}
	int lz[zsx_values];
	for (s = 0; s < zsx_values; s++)
		lz[s] = 0;


	zsx_del(codi);
	zsx_del(dico);
	zsx_del(codes);

	return result;
}


void BackLine() {
	CONSOLE_SCREEN_BUFFER_INFO *info = zsx_new(CONSOLE_SCREEN_BUFFER_INFO, 1);
	GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), info);
	COORD XY;
	XY.X = 0;
	XY.Y = info->dwCursorPosition.Y;
	SetConsoleCursorPosition(GetStdHandle(STD_OUTPUT_HANDLE), XY);
}

int zsx_bytes_read;

void CALLBACK FIO(DWORD E1, DWORD N2, LPOVERLAPPED ol) {
	zsx_bytes_read = N2;
}

int test(char *filename) {

	OVERLAPPED ol;
	ol.Offset = 0;
	ol.OffsetHigh = 0;

	HANDLE hFile = CreateFile(
		filename,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
		NULL);

	if (hFile == INVALID_HANDLE_VALUE) {
		printf("Can't read file!\n");
		return 0;
	};

	int encoded = 0;
	DWORD file_high;
	int file_size = GetFileSize(hFile, &file_high);

	zsx_bytes_buffer = zsx_new(char, zsx_chunk);
	zsx_result_buffer = zsx_new(char, zsx_chunk);

	char *outfilename = zsx_new(char, 256);
	strcpy(outfilename, filename);
	strcat(outfilename, ".zsx");
	FILE *f = fopen(outfilename, "wb");

	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};

	printf("encoding...\n");

	COMPRESSOR_HANDLE Compressor = NULL;
	CreateCompressor(3, NULL, &Compressor);


	while (encoded < file_size) {
		ReadFileEx(hFile, zsx_bytes_buffer, zsx_chunk, &ol, FIO);
		SleepEx(3000, TRUE);
		encoded += zsx_bytes_read;
		ol.Offset += zsx_bytes_read;
		int prev_len = zsx_bytes_read;

#ifdef ZSX	



		size_t csize;
		Compress(Compressor, zsx_bytes_buffer, zsx_bytes_read, NULL, 0, &csize);
		Compress(Compressor, zsx_bytes_buffer, zsx_bytes_read, zsx_result_buffer, csize, &csize);

		*((size_t *)zsx_result_buffer - 1) = csize;
		byte *data = zsx_encode(zsx_result_buffer);
		size_t len_zsx = zsx_len(data);


		while (len_zsx < prev_len)
		{
			prev_len = len_zsx;

			Compress(Compressor, zsx_bytes_buffer, len_zsx, NULL, 0, &csize);
			Compress(Compressor, zsx_bytes_buffer, len_zsx, zsx_result_buffer, csize, &csize);
			*((size_t *)zsx_result_buffer - 1) = csize;
			data = zsx_encode(zsx_result_buffer);
			len_zsx = zsx_len(data);

		}

		//printf("final size: %ld.\n", len_zsx);
		fwrite(&len_zsx, sizeof(size_t), 1, f);
		fwrite(data, len_zsx, 1, f);
		BackLine();
		printf("%.2f%%", (double)encoded / (double)file_size * 100.0);
#else
		memcpy(result_buffer, bytes_buffer, bytes_read);
		byte * data = zsx_encode(zsx_result_buffer, bytes_read);
		size_t len_zsx = zsx_len(data);

		byte *decompressed = zsx_decode(data, len_zsx);
		puts(decompressed);
		fwrite(decompressed, zsx_len(decompressed), 1, f);
		BackLine();
		printf("%.2f%%", (double)encoded / (double)file_size * 100.0);

#endif
	}

	CloseCompressor(Compressor);

	fclose(f);
	CloseHandle(hFile);

	return 1;
}


int main(int argc, char **argv) {
	if(argc<2)
		printf("Usage : %s filename\n", argv[0]);
	else
	test(argv[1]);
	return 0;
}
