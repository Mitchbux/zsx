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

int zsx_Initial[zsx_top][zsx_items];
int zsx_Indexes[zsx_top];
int zsx_Unknown[zsx_top];

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

	int last = 0;
	
	
	byte *exists = zsx_new(byte, zsx_values);
	for(s=0;s<zsx_values;s++)
		exists[s]=0;
	
	for(s=0;s<zsx_top;s++)
		zsx_Indexes[s]=0;
	
	while (reader->start <= len) {
		int z = zsx_read_value(reader, zsx_bits);
		int s = zsx_read_value(reader, zsx_bits);
		int x = zsx_read_value(reader, zsx_bits);

		int hash = (z<<zsx_bits)^(s<<zsx_half)^x;
		int index = zsx_Indexes[hash];
		zsx_write_bit(bytes, exists[s] ? 1 : 0);

		if (exists[s])
		{
			zsx_write_value(bytes, s, zsx_bits);
			zsx_write_bit(bytes, index?1:0);
			if(index==0)
			{
				zsx_write_value(bytes, z, zsx_bits);
				zsx_write_value(bytes, x, zsx_bits);			
				zsx_Initial[s][0] = z;
				zsx_Initial[s][1] = s;
				zsx_Initial[s][2] = x;
				zsx_Indexes[hash] = ++last;
				zsx_Unknown[last] = 0;
			}else
			{
				zsx_write_value(bytes, index-1, zsx_lf(last));
			}
		}else
		{
			
			zsx_write_bit(bytes, index?1:0);
			if (index)
			{
				zsx_write_value(bytes, index-1, zsx_bits);
				zsx_write_value(bytes, s, zsx_bits);
			}else
			{
				zsx_write_bit(bytes, 0);
				exists[s] = ++last;
				zsx_Initial[s][0] = z;
				zsx_Initial[s][1] = s;
				zsx_Initial[s][2] = x;
				zsx_Indexes[hash] = last;
				zsx_Unknown[last] = 1;
			}
		}

		if(last==zsx_win)
		{
			for(s=0;s<last;s++)
			{
				if(zsx_Unknown[s])
				{
					zsx_write_value(bytes, zsx_Initial[s][0], zsx_bits);
					zsx_write_value(bytes, zsx_Initial[s][1], zsx_bits);
					zsx_write_value(bytes, zsx_Initial[s][2], zsx_bits);
				}
				int h = (zsx_Initial[s][0] << zsx_bits) ^ (zsx_Initial[s][1] << zsx_half) ^ zsx_Initial[s][2];
				zsx_Indexes[h]=0;
				exists[zsx_Initial[s][1]]=0;
			}
			last = 0;
		}			
	}
			for(s=0;s<last;s++)
			{
				if(zsx_Unknown[s])
				{
					zsx_write_value(bytes, zsx_Initial[s][0], zsx_bits);
					zsx_write_value(bytes, zsx_Initial[s][1], zsx_bits);
					zsx_write_value(bytes, zsx_Initial[s][2], zsx_bits);
				}
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

	int items = size/3+((size%3)?1:0);
	int *itemat = zsx_new(int, zsx_values);
	int **itemin = zsx_new(int*, zsx_values);
	int **sample = zsx_new(int*, zsx_values);
	int *samplecount = zsx_new(int, zsx_values);
	int *initial = zsx_new(int, zsx_values);
	
	for(x = 0; x < zsx_values; x++)
	{
		itemin[x] = zsx_new(int, 128*1024);
		sample[x] = zsx_new(int, 128*1024);
		samplecount[x] = 0;
		itemat[x]=0;
		initial[x]=-1;
	}
	
	int decoded = 0;
	int last = 0;

		puts("decoding...");
	
	while(decoded < size)
	{
		if(zsx_read_bit(bytes))
		{
			int n = zsx_read_value(bytes, zsx_bits);			
			itemin[n][samplecount[n]] = decoded;
			sample[n][samplecount[n]++] = -1;
		}else
		{
			if(zsx_read_bit(bytes))
			{
				int n = zsx_read_value(bytes, zsx_bits);
				itemin[n][samplecount[n]] = decoded;
				sample[n][samplecount[n]++] = zsx_read_value(bytes, zsx_bits);
			}else
			if(zsx_read_bit(bytes))
			{
				int index = zsx_read_value(bytes, zsx_lf(last));
				z = zsx_read_value(bytes, zsx_bits);
				s = zsx_read_value(bytes, zsx_bits);
				x = zsx_read_value(bytes, zsx_bits);
				result[itemat[index]] = z;
				result[itemat[index]+1] = s;
				result[itemat[index]+2] = x;
				if(initial[index]==-1)
				{
					initial[index] = (z<<zsx_bits)^x;
				}
				itemat[index] = decoded;
			}else
			{
				itemat[last++] = decoded;
			}
		}
		decoded++;
		decoded++;
		decoded++;
		if(last==zsx_win)
		{
			for(int index=0;index<last;index++)
			{
				z = zsx_read_value(bytes, zsx_bits);
				s = zsx_read_value(bytes, zsx_bits);
				x = zsx_read_value(bytes, zsx_bits);
				int hash = (z<<zsx_bits)^(s<<zsx_half)^x;
				if(initial[index]>=0)
				{
					hash = initial[index]^(s<<zsx_half);
				}

				result[itemat[index]] = z;
				result[itemat[index]+1] = s;
				result[itemat[index]+2] = x;
				for(int n =0;n<samplecount[s];n++)
				{
					int smpl = sample[s][n];
					if (smpl==-1)
					{
						if(initial[index]>=0)
						{
							result[itemin[s][n]] = initial[index]>>zsx_bits;
							result[itemin[s][n]+1] = s;
							result[itemin[s][n]+2] = initial[index]&255;
						}else
						{
							result[itemin[s][n]] = z;
							result[itemin[s][n]+1] = s;
							result[itemin[s][n]+2] = x;
						}
					}else
					{
						result[itemin[s][n]] = (hash>>zsx_bits) ^ (smpl>>zsx_half);
						result[itemin[s][n]+1] = smpl;
						result[itemin[s][n]+2] = (hash&255) ^ ((smpl&15)<<zsx_half);
					}
				}
				initial[index] = -1;
				samplecount[s] = 0;
			}
			last = 0;
		}
	}
	for(int index=0;index<last;index++)
	{
		z = zsx_read_value(bytes, zsx_bits);
		s = zsx_read_value(bytes, zsx_bits);
		x = zsx_read_value(bytes, zsx_bits);
		int hash = (z<<zsx_bits)^(s<<zsx_half)^x;
		if(initial[index]>=0)
		{
			hash = initial[index]^(s<<zsx_half);
		}
		result[itemat[index]] = z;
		result[itemat[index]+1] = s;
		result[itemat[index]+2] = x;
		
		for(int n =0;n<samplecount[s];n++)
		{
			int smpl = sample[s][n];
			if (smpl==-1)
			{
				if(initial[index]>=0)
				{
					result[itemin[s][n]] = initial[index]>>zsx_bits;
					result[itemin[s][n]+1] = s;
					result[itemin[s][n]+2] = initial[index]&255;
				}else
				{
					result[itemin[s][n]] = z;
					result[itemin[s][n]+1] = s;
					result[itemin[s][n]+2] = x;
				}
			}else
			{
				result[itemin[s][n]] = (hash>>zsx_bits) ^ (smpl>>zsx_half);
				result[itemin[s][n]+1] = smpl;
				result[itemin[s][n]+2] = (hash&255) ^ ((smpl&15)<<zsx_half);
			}
		}
	}
	result[decoded++]=0;
	zsx_del(bytes);
	for(x=0;x<zsx_values;x++)
	{
		zsx_del(itemin[x]);
		zsx_del(sample[x]);
	}
	zsx_del(itemin);
	zsx_del(sample);
	zsx_del(itemat);
	zsx_del(samplecount);
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

		data = zsx_encode(zsx_result_buffer);
		len_zsx = zsx_len(data);

		byte *decompressed = zsx_decode(data);
		
		memcpy(zsx_result_buffer, zsx_bytes_buffer, zsx_len(decompressed));
		zsx_len(zsx_result_buffer) = zsx_len(decompressed);
		decompressed = zsx_decode(zsx_result_buffer);
		
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
