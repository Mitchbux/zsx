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
typedef uint16_t ushort;

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
#define zsx_extend(m) m = zsx_extend(m, zsx_len(m) * 2);

/*--------- bit reader/writer---------*/

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
	int canextend;
} zsx_writer_t;

void zsx_write_bit(zsx_writer_t *p, int v) {
	p->bits += (v & 1) << (8 - (++p->power));
	if (p->power >= 8) {
		p->data[p->start++] = p->bits;
		p->power = 0;
		p->bits = 0;
		if (p->canextend && ((zsx_len(p->data)) - p->start) < sizeof(int))
			zsx_extend(p->data);
	}
}

void zsx_write_value(zsx_writer_t *p, int v, int n) {
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

int zsx_read_value(zsx_reader_t *r, int n) {
	int v = 0;
	while (n > 0) {
		n--;
		v = v << 1;
		v += zsx_read_bit(r);
	}
	return v;
}

#define zsx_extendable 1
#define zsx_statically 0

zsx_writer_t *zsx_writer(byte *out, int ext) {
	zsx_writer_t *r = zsx_new(zsx_writer_t, 1);
	r->power = 0;
	r->bits = 0;
	r->start = 0;
	r->data = (char *)out;
	r->canextend = ext;
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

int zsx_lf(uint64_t n) {
	int m = 1;
	while ((uint64_t)(1 << m) < n)
		m++;
	return m;
}

#define zsx_chunk 16*1024*1024

byte *zsx_bytes_buffer;
byte *zsx_result_buffer;
byte *zsx_compress_buffer;

#define zsx_init zsx_result_buffer = zsx_new(byte, zsx_chunk); zsx_bytes_buffer = zsx_new(byte, zsx_chunk); zsx_compress_buffer = zsx_new(byte, zsx_chunk*2);
#define zsx_exit zsx_del(zsx_result_buffer);  zsx_del(zsx_bytes_buffer); zsx_del(zsx_compress_buffer);

/****** zsx ******/
byte *zsx_encode(byte *data) {
	int length = zsx_len(data);
	int z, s, x, k;
	byte *result = zsx_result_buffer;
	zsx_writer_t *compress = zsx_writer(zsx_compress_buffer, zsx_statically);
	zsx_writer_t *bytes = zsx_writer(zsx_bytes_buffer, zsx_statically);
	zsx_reader_t *reader = zsx_reader(data);
	zsx_write_value(compress, length, 32);

	int full=0, next=0, stop=0;
	int *stacker = zsx_new(int, 256*256);
	int *reverse = zsx_new(int, 256*256);
	while(stop<length){
		zsx_clear(stacker);
		zsx_clear(reverse);
		next=0;
	while(stop<length)
	{
		x=(data[stop++]<<8)^data[stop++];
		zsx_write_bit(compress, stacker[x]?1:0);
		if(stacker[x])
		{
			zsx_write_value(bytes, x, 16);
			reverse[x]=0;
		}
		else 
		{
		stacker[x]=++next;
		reverse[x]=1;
		}
		if(next==4096)break;
	}
	}
	for(s=0;s<256*256;s++){
	if(stacker[s])
	{
		zsx_write_bit(compress, reverse[s]);
		if(reverse[s])zsx_write_value(compress, s, 16);
		zsx_write_value(compress, stacker[s], zsx_lf(next));
	}}
	zsx_del(stacker);


	zsx_flushWrite(compress);
	COMPRESSOR_HANDLE Compressor = NULL;
	CreateCompressor(3, NULL, &Compressor);
	size_t csize;
	Compress(Compressor, compress->data, compress->start, NULL, 0, &csize);
	byte *mszip = zsx_new(byte, csize);
	Compress(Compressor, compress->data, compress->start, mszip, csize, &csize);
	CloseCompressor(Compressor);

	Compressor = NULL;
	CreateCompressor(2, NULL, &Compressor);

	size_t csize2;
	Compress(Compressor, bytes->data, bytes->start, NULL, 0, &csize2);
	byte *mszip2 = zsx_new(byte, csize2);
	Compress(Compressor, bytes->data, bytes->start, mszip2, csize2, &csize2);
	CloseCompressor(Compressor);

	memcpy(result, mszip, csize);
	result+=csize;
	memcpy(result, mszip2, csize2);
	result+=csize2;
	zsx_del(mszip);
	zsx_del(compress);

	zsx_len(zsx_result_buffer) = result-zsx_result_buffer;
	return zsx_result_buffer;
}

byte *zsx_decode(byte *data) {
	DECOMPRESSOR_HANDLE Decompressor = NULL;
	CreateDecompressor(2, NULL, &Decompressor);

	int len = zsx_len(data);
	size_t dsize;
	Decompress(Decompressor, data, len, NULL, 0, &dsize);
	byte *buffer = zsx_new(byte, dsize);
	Decompress(Decompressor, data, len, buffer, dsize, &dsize);

	CloseDecompressor(Decompressor);

	zsx_reader_t *key = zsx_reader(buffer);
	int z, s, x, length = zsx_read_value(key, 32);
	byte * result = zsx_compress_buffer;
	int **dico = zsx_new(int*, 256*256);
	for (s = 0; s < 256*256; s++)
		dico[s] = zsx_new(int, 4096);
	int *indexes = zsx_new(int, 256*256);
	int *reverse = zsx_new(int, 256*256);
	int *counter = zsx_new(int, 256*256);
	zsx_clear(indexes);
	zsx_clear(reverse);
	zsx_clear(counter);

	int decoded = 0;
	int last = 0, left = 0, next = 0;
	while (decoded < length)
	{
		if (zsx_read_bit(key))
		{
			int returns = reverse[zsx_read_value(key, zsx_lf(next))+1];
			x = dico[returns][zsx_read_value(key, zsx_lf(counter[returns]))];
		}
		else
		{
			x = zsx_read_value(key, 16);
			dico[last][counter[last]++] = x;
			if (indexes[last] == 0)
			{
				indexes[last] = next == 64 ? ++left : ++next;
				if (next == 64)
				{
					indexes[reverse[left]] = 0;
					counter[reverse[left]] = 0;
				}
				if (left == 64)left = 0;
				reverse[indexes[last]] = last;
			}

		}
		result[decoded++] = x>>8;
		result[decoded++] = x&255;
		last = x&255;
	}

	for (s = 0; s < 256*256; s++)
		zsx_del(dico[s]);
	zsx_del(dico);
	zsx_del(indexes);
	zsx_del(reverse);
	zsx_del(counter);

	memcpy(zsx_result_buffer, zsx_compress_buffer, length);
	zsx_len(zsx_result_buffer) = length;
	zsx_result_buffer[length] = 0;
	return zsx_result_buffer;
}

int zsx_bytes_read;

void CALLBACK FIO(DWORD E1, DWORD N2, LPOVERLAPPED ol) {
	zsx_bytes_read = N2;
}


int test(char *filename) {

	// *** encoding file chunk by chunk ***

	//win32 file handling

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

	char *outfilename = zsx_new(char, 256);
	strcpy(outfilename, filename);
	strcat(outfilename, ".zsx");

	FILE *f = fopen(outfilename, "wb");

	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};


	printf("\nEncoding...\n");

	zsx_init
		while (encoded < file_size) {

			//win32 buffered read
			ReadFileEx(hFile, zsx_result_buffer, zsx_chunk, &ol, FIO);
			SleepEx(INFINITE, TRUE);
			encoded += zsx_bytes_read;
			ol.Offset += zsx_bytes_read;
			zsx_len(zsx_result_buffer) = zsx_bytes_read;
			zsx_encode(zsx_result_buffer);
			size_t len_zsx = zsx_len(zsx_result_buffer);
			int prev_len = len_zsx + 1;
			for (int z = 0; prev_len > len_zsx; z++)
			{
				prev_len = len_zsx;
				zsx_encode(zsx_result_buffer);
				len_zsx = zsx_len(zsx_result_buffer);
				//printf("%d ", len_zsx);
			}
			//c-like file writing
			fwrite(&len_zsx, sizeof(size_t), 1, f);
			fwrite(zsx_result_buffer, len_zsx, 1, f);
			printf("\r%.2f%%", (double)encoded / (double)file_size * 100.0);
		}
	zsx_exit
		fclose(f);
	CloseHandle(hFile);


	printf("\nCompressed to %s.\n", outfilename);

	return 1;
}

int testdec(char *filename) {

	//*** Testing Encoding and Decoding ***

	//win32 file handling
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

	char *outfilename = zsx_new(char, 256);
	strcpy(outfilename, filename);
	strcat(outfilename, ".dec");
	FILE *f = fopen(outfilename, "wb");

	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};

	//zsx library has a few set of macros.
	//this one takes care of everything about initialization
	zsx_init

		while (encoded < file_size) {
			ReadFileEx(hFile, zsx_result_buffer, zsx_chunk, &ol, FIO);
			SleepEx(INFINITE, TRUE);
			encoded += zsx_bytes_read;
			ol.Offset += zsx_bytes_read;

			printf("\nEncoding twice...\n");

			//zsx_len is a dereferenced pointer can be used as left-op or right-op and describes the size of allocated memory 
			zsx_len(zsx_result_buffer) = zsx_bytes_read;
			byte * data = zsx_encode(zsx_result_buffer);
			//data = zsx_encode(data);

			printf("\nDecoding twice...\n");

			byte *decompressed = zsx_decode(data);
			//decompressed = zsx_decode(decompressed);

			puts(decompressed);
			//zsx_len contains the size of the data but not what was actually allocated at first. 
			// !! Caution !! do not use these macro for any other context than compression.
			fwrite(decompressed, zsx_len(decompressed), 1, f);
		}
	zsx_exit
		fclose(f);
	CloseHandle(hFile);

	printf("Original data decoded to file %s.\n", outfilename);


	return 1;
}


int main(int argc, char **argv) {

	if (argc < 2)
		printf("Usage : %s [-d] filename\n", argv[0]);

	else if (argc == 2)
		test(argv[1]);

	else if (argv[1][0] == '-' && argv[1][1] == 'd')
		testdec(argv[2]);
	return 0;

}
