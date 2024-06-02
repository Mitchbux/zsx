

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

//Logarithm function : returns the number of bits needed to store a maximum value not reached
int zsx_lf(uint64_t n) {
	int m = 1;
	while ((uint64_t)(1 << m) < n)
		m++;
	return m;
}

//merge sort : writes a bit when compairing two values so the algorithme can be done backwards

void zsx_merge (zsx_writer_t *w, int *a, int n, int m) {
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

void zsx_merge_sort (zsx_writer_t *w, int *a, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    zsx_merge_sort(w, a, m);
    zsx_merge_sort(w, a + m, n - m);
    zsx_merge(w, a, n, m);
}

void zsx_merge_read (zsx_reader_t *r, int *a, int n, int m) {
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

void zsx_merge_sort_read (zsx_reader_t *r, int *a, int n) {
    if (n < 2)
        return;
    int m = n / 2;
    zsx_merge_sort_read(r, a, m);
    zsx_merge_sort_read(r, a + m, n - m);
    zsx_merge_read(r, a, n, m);
}

#define zsx_chunk 4096*4096

byte *zsx_result_buffer;
byte *zsx_bytes_buffer;
byte *zsx_key_buffer;

#define zsx_init zsx_result_buffer = zsx_new(byte, zsx_chunk); zsx_bytes_buffer = zsx_new(byte, zsx_chunk); zsx_key_buffer = zsx_new(byte, zsx_chunk);
#define zsx_exit zsx_del(zsx_result_buffer);  zsx_del(zsx_bytes_buffer);  zsx_del(zsx_key_buffer); 

/****** zsx ******/
byte *zsx_encode(byte *data) {
	int length = zsx_len(data);
	int z, s, x;
	byte *result = zsx_result_buffer;
	byte *bytes = zsx_bytes_buffer;
	zsx_writer_t*key = zsx_writer(zsx_key_buffer, zsx_statically);
	
	zsx_write_value(key, length, 32);

	int full = 0, stop=0, next=0, last=0, start=0;

	int *stacker = zsx_new(int, 0x100);
	int *counter = zsx_new(int, 0x100);
	int *reverse = zsx_new(int, 0x100);
	int *indexes = zsx_new(int, 0x100);
	zsx_clear(stacker);
	zsx_clear(counter);
	while(stop<length)
	{
		z=data[stop++];
		zsx_write_bit(key, stacker[z]?1:0);
		if(stacker[z]==0)
		{
			stacker[z]=next?indexes[--next]:++last;
			if(next<64)
			for(s=0;s<256;s++)
			if(counter[s])indexes[next++]=s, counter[s]=0;
			zsx_write_value(key, z, 8);
			if(stacker[z]<last)
				stacker[reverse[stacker[z]]]=0;
			reverse[stacker[z]]=z;
		}else
			bytes[start++]=stacker[z], counter[stacker[z]]++;

	}
	zsx_del(stacker);
	zsx_del(counter);
	zsx_del(reverse);
	zsx_del(indexes);

	COMPRESSOR_HANDLE Compressor = NULL;
	CreateCompressor(3, NULL, &Compressor);
	size_t csize;
	Compress(Compressor, bytes, start, NULL, 0, &csize);
	result+=sizeof(size_t);
	Compress(Compressor, bytes, start, result, csize, &csize);
	*(size_t*)zsx_result_buffer=csize;
	zsx_flushWrite(key);
	size_t ksize;
	Compress(Compressor, key->data, key->start, NULL, 0, &ksize);
	Compress(Compressor, key->data, key->start, result+csize, ksize, &ksize);
	CloseCompressor(Compressor);
	zsx_len(zsx_result_buffer) = csize+ksize+sizeof(size_t);
	return zsx_result_buffer;
}

byte *zsx_decode(byte *data) {
	size_t xpress_len = *(size_t*)data;
	byte * xpress = data + sizeof(size_t);
	size_t dsize;

	DECOMPRESSOR_HANDLE Decompressor = NULL;
	CreateDecompressor(3, NULL, &Decompressor);
	Decompress(Decompressor, xpress, xpress_len, NULL, 0, &dsize);
	byte *buffer = zsx_new(byte, dsize);
	Decompress(Decompressor, xpress, xpress_len, buffer, dsize, &dsize);

	int len = zsx_len(data) - sizeof(size_t) - xpress_len;
	byte *keyxpress = data + sizeof(size_t) + xpress_len;
	size_t ksize;
	Decompress(Decompressor, keyxpress, len, NULL, 0, &ksize);
	byte *key_buffer = zsx_new(byte, ksize);
	Decompress(Decompressor, keyxpress, len, key_buffer, ksize, &ksize);
	CloseDecompressor(Decompressor);

	zsx_reader_t *key = zsx_reader(key_buffer);
	zsx_reader_t *bytes = zsx_reader(buffer);
	int z, s, x, length = zsx_read_value(key, 32);
	byte * result = zsx_bytes_buffer;
	int *indexof = zsx_new(int, 256);
	int *indexes = zsx_new(int, 256);
	int *stacker = zsx_new(int, 256);
	int *reverse = zsx_new(int, 256);
	int *values = zsx_new(int, 256);

	int decoded = 0;
	int next, start = 0;
	while(decoded < length)
	{

		zsx_clear(stacker);
		
		int stop = zsx_read_value(key, zsx_lf(length));
		printf("%d ", stop);
		next = 0;
		while (decoded < stop)
		{
			next=zsx_read_value(key, 8);

			//we rebuild the index initial order
			for(s=0;s<next;s++)
				indexes[s]=s;
			zsx_merge_sort_read(key, indexes, next);
			for(s=0;s<next;s++)
				indexof[indexes[s]]=s;

			int complete = zsx_read_bit(key);

			if(zsx_read_bit(key))
			{
				x = zsx_read_value(bytes, 8);
				printf("%d ", x);
				//we hold the existence of each value
				stacker[x]=1;

				//the indexes are reordered later
				result[decoded++]=x;

			}else
			{
				if(complete)
					result[decoded++]=-1;
				else 
				{
					x=zsx_read_value(bytes, 8);
					stacker[x]=1;
					result[decoded++]=x;
				}
			}
		}
		printf("%d ", decoded);

		//we have all the different values used by indexes
		for(x=0,s=0;s<256;s++)
			if(stacker[s])
			{
				values[x++]=s;
				printf("%c ", s);
				reverse[s]=x-1;
			}
		
		//we find the index of appearance from the decoded value and the initial value from the ordered indexof
		for(next = 0, s = start; s < decoded; s++)
		{
			if(result[s]>-1)
			{
		   		result[s] = values[indexof[reverse[result[s]]]];
			}else
			{
				result[s] = values[indexof[next++]];
			}
		}

		//the next iteration will decode further
		start = decoded;
    }

    zsx_del(indexes);
    zsx_del(indexof);
	zsx_del(stacker);
	zsx_del(reverse);
    zsx_del(values);

    zsx_del(key_buffer);
	zsx_del(buffer);

	memcpy(zsx_result_buffer, zsx_bytes_buffer, length);
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
			zsx_encode(zsx_result_buffer);
			size_t len_zsx = zsx_len(zsx_result_buffer);
			int prev_len = len_zsx + 1;
			for (int z = 0; prev_len>len_zsx; z++)
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

