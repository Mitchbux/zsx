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

#define zsx_chunk_bits 30
#define zsx_chunk 1 << zsx_chunk_bits

#define zsx_bits 8
#define zsx_values 256
#define zsx_items 3
#define zsx_half 4
#define zsx_top 256*256
#define zsx_win 128*256

int zsx_Initial[zsx_top];
int zsx_Indexes[zsx_top];
int zsx_Next[zsx_values];
int zsx_NextIndex;

char *zsx_bytes_buffer;
char *zsx_result_buffer;


/****** zsx ******/
byte *zsx_encode(byte *data) {
	int len = zsx_len(data);
	int z, s, x;
	byte *result;
	zsx_writer_t *bytes = zsx_writer(zsx_bytes_buffer);
	zsx_reader_t *reader = zsx_reader(data);
	zsx_write_value(bytes, len, 32);

	printf(".");

	int in = 0;
	int last = 0;
	int *exists = zsx_new(int, zsx_values*16);
	for (s = 0; s < zsx_values*16; s++)
	{
		exists[s] = 0;
	}
	for(s=0;s<zsx_top;s++)
		zsx_Indexes[s]=0;
	
	zsx_NextIndex = 0;
	
	while (in < len) {
		int z = data[in++];
		int s = data[in++];
		int x = data[in++];

		int hash = (z<<2)^(s<<1)^x;
		int index = zsx_Indexes[hash];
		zsx_write_bit(bytes, index?1:0);

		if (index)
		{
			zsx_write_bit(bytes, zsx_Initial[hash] == s ? 1 : 0);
			if (zsx_Initial[hash] == s )
			{
				zsx_write_value(bytes, s, zsx_bits);
			}
			else
			{
				zsx_write_value(bytes, zsx_Initial[hash], zsx_bits);
				zsx_write_value(bytes, s, zsx_bits);
			}
		}else
		{
			zsx_write_value(bytes, s, zsx_bits);
			if (exists[s])
			{
				zsx_write_value(bytes, exists[s], zsx_bits+2);
				zsx_Next[zsx_NextIndex++]=zsx_Indexes[exists[s]];
				zsx_Indexes[exists[s]] = 0;
			}
			zsx_Indexes[hash] = zsx_NextIndex? zsx_Next[--zsx_NextIndex]:++last;
			zsx_Initial[hash] = s;
			exists[s]=hash;
		}
	}

	for (s = 0; s < zsx_values*4; s++)
	{
		zsx_write_bit(bytes, zsx_Indexes[s] ? 1 : 0);
		if (zsx_Indexes[s])
			zsx_write_value(bytes, zsx_Indexes[s] - 1, zsx_lf(last));
	}


	zsx_flushWrite(bytes);
	
	printf("%d ", bytes->start);
	
	result = zsx_result_buffer;
	zsx_len(result)=bytes->start;
	memcpy(result, bytes->data, bytes->start);
	
	zsx_del(reader);
	zsx_del(bytes);
	
	return result;
}

byte *zsx_decode(byte *data) {
	zsx_reader_t *bytes = zsx_reader(data);
	unsigned int z, s, x, c, b, size = zsx_read_value(bytes, 32);

	byte * result = zsx_bytes_buffer;
	zsx_len(zsx_bytes_buffer) = size;


	int **dico = zsx_new(int*, zsx_values);
	for (s = 0; s < zsx_values; s++)
	{
		dico[s] = zsx_new(int, 4);
	}


	int decoded = 0;
	int last = 0;

		puts("decoding...");
	
	while(decoded < size)
	{
		if(zsx_read_bit(bytes))
		{
			int n = zsx_read_value(bytes, zsx_bits);
			result[decoded++] = dico[n][0];
			result[decoded++] = dico[n][1];
			result[decoded++] = dico[n][2];
		}else
		{
			if(zsx_read_bit(bytes))
			{
				int n = zsx_read_value(bytes, zsx_bits);
				s = zsx_read_value(bytes, zsx_bits);
				int h = (dico[n][0] << zsx_bits) ^ (dico[n][1] << zsx_half) ^ dico[n][2];
				result[decoded++] = (h >> zsx_bits) ^ (s >> zsx_half);
				result[decoded++] = s;
				result[decoded++] = (h & 255) ^ ((s & 15) << zsx_half);
			}else
			{
				z = zsx_read_value(bytes, zsx_bits);
				s = zsx_read_value(bytes, zsx_bits);
				x = zsx_read_value(bytes, zsx_bits);
				result[decoded++] = z;
				result[decoded++] = s;
				result[decoded++] = x;
				
					dico[s][0] = z;
					dico[s][1] = s;
					dico[s][2] = x;
			}
		}
	}
	result[decoded++]=0;
	zsx_del(bytes);
	for (s = 0; s < zsx_values; s++)
	{
		zsx_del(dico[s]);
	}
	zsx_del(dico);
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



		size_t csize;
		Compress(Compressor, zsx_bytes_buffer, zsx_bytes_read, NULL, 0, &csize);
		Compress(Compressor, zsx_bytes_buffer, zsx_bytes_read, zsx_result_buffer, csize, &csize);
		printf("%d ", csize);
		
		zsx_len(zsx_result_buffer) = csize;
		byte *data = zsx_encode(zsx_result_buffer);
		size_t len_zsx = zsx_len(data);


		while (len_zsx < prev_len)
		{
			prev_len = len_zsx;
			data = zsx_encode(zsx_result_buffer);
			len_zsx = zsx_len(data);
		}

		//printf("final size: %ld.\n", len_zsx);
		fwrite(&len_zsx, sizeof(size_t), 1, f);
		fwrite(data, len_zsx, 1, f);
		BackLine();
		printf("%.2f%%", (double)encoded / (double)file_size * 100.0);
	}

	CloseCompressor(Compressor);

	fclose(f);
	CloseHandle(hFile);

	return 1;
}

int testdec(char *filename) {

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
	strcat(outfilename, ".dec");
	FILE *f = fopen(outfilename, "wb");

	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};

	printf("encoding...\n");

	COMPRESSOR_HANDLE Compressor = NULL;
	CreateCompressor(3, NULL, &Compressor);


	while (encoded < file_size) {
		ReadFileEx(hFile, zsx_result_buffer, zsx_chunk, &ol, FIO);
		SleepEx(3000, TRUE);
		encoded += zsx_bytes_read;
		ol.Offset += zsx_bytes_read;
		int prev_len = zsx_bytes_read;

		zsx_len(zsx_result_buffer) = zsx_bytes_read;
		byte * data = zsx_encode(zsx_result_buffer);
		size_t len_zsx = zsx_len(data);

		//data = zsx_encode(zsx_result_buffer);
		//len_zsx = zsx_len(data);

		byte *decompressed = zsx_decode(data);
		
		//memcpy(zsx_result_buffer, zsx_bytes_buffer, zsx_len(decompressed));
		//zsx_len(zsx_result_buffer) = zsx_len(decompressed);
		//decompressed = zsx_decode(zsx_result_buffer);
		
		puts(decompressed);
		fwrite(decompressed, zsx_len(decompressed), 1, f);
		BackLine();
		printf("%.2f%%", (double)encoded / (double)file_size * 100.0);
	}

	CloseCompressor(Compressor);

	fclose(f);
	CloseHandle(hFile);

	return 1;
}

int main(int argc, char **argv) {
	if(argc<2)
		printf("Usage : \n %s filename --- compress the file\n %s -d filename.zsx --- decompress the archive\n", argv[0], argv[0]);
	else if(argc==2)
	test(argv[1]);
	else if(argv[1][0]=='-' && argv[1][1]=='d')
	testdec(argv[2]);
	return 0;
}
