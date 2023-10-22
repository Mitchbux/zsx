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

int zsx_lf(unsigned long n) {
  int m = 1;
  while ((unsigned long)(1 << m) < n)
    m++;
  return m;
}

#define zsx_big 1024*1024*1000
#define zsx_sml 125*1000
#define zsx_bit 8
#define zsx_hlf 4
#define zsx_min 1024
#define zsx_hsh 256*256
#define zsx_top 256*64


byte *zsx_bytes_buffer;
byte *zsx_result_buffer;
int zsx_chunk;

#define zsx_init zsx_bytes_buffer = zsx_new(byte, zsx_chunk); zsx_result_buffer = zsx_new(byte, zsx_chunk);


/****** zsx ******/
byte *zsx_encode(byte *data) {
  int length = zsx_len(data);
  int z, s, x, k, v, n, a, b;
  byte *result;
  zsx_writer_t *bytes = zsx_writer(zsx_bytes_buffer, zsx_extendable);
  zsx_write_value(bytes, length, 32);

  printf(".");

  int *indexes = zsx_new(int, zsx_hsh);
  int *initial = zsx_new(int, zsx_hsh);
  int *unknown = zsx_new(int, zsx_hsh);

int *idx = zsx_new(int, zsx_hsh);
  int last = 0,next = 0, stop, full = 0;
while (stop < length) {
    z = data[stop++];
    s = data[stop++];
    x = data[stop++];
    int hash = (z << zsx_bit) ^ (s << zsx_hlf) ^ x;
    indexes[hash]=1;
}
for(s=0;s<zsx_hsh&&last<zsx_top;s++)
{
	zsx_write_bit(bytes, indexes[s]);
	if(indexes[s])indexes[s]=++last;
}
int max = s - 1;
for(;s<zsx_hsh;s++)
	indexes[s]=0;
  zsx_clear(idx);
    int won = 0;
 while (full < length) {
    z = data[full++];
    s = data[full++];
    x = data[full++];
    int hash = (z << zsx_bit) ^ (s << zsx_hlf) ^ x;
    won += zsx_bit + zsx_bit + zsx_bit;
	if (idx[hash])
	{
		zsx_write_bit(bytes, 1);
		zsx_write_value(bytes, idx[hash]-1, zsx_lf(next));
		won -= zsx_lf(last);
	}
    else
    {
		zsx_write_bit(bytes, 0);
		zsx_write_bit(bytes, hash>max?1:0);
		if(hash>max)
		{
			zsx_write_value(bytes, hash, zsx_bit+zsx_bit);
			won -= zsx_bit+zsx_bit;
		}else
        {
			zsx_write_value(bytes, indexes[hash]-1, zsx_lf(last));
			won -= zsx_lf(last);
	    }
		idx[hash]=++next;
		unknown[next]=hash;
        
    }
	if(initial[hash]!=s)
	{
      zsx_write_value(bytes, s, zsx_bit);
      won -= zsx_bit;
	}
    initial[hash] = s;
	if(won>zsx_hsh)
	{
		for(;next>zsx_min;next--)
		{
			idx[unknown[next]]=0;
		}
		won=0;
	}

  }

  zsx_del(indexes);
  zsx_del(initial);
  zsx_del(unknown);
  zsx_del(idx);
  
  zsx_flushWrite(bytes);
  result = zsx_result_buffer;
  zsx_len(result) = bytes->start;
  memcpy(result, bytes->data, bytes->start);

  //zsx_del(reader);
  zsx_del(bytes);
  return result;
}

byte *zsx_decode(byte *data) {
  zsx_reader_t *bytes = zsx_reader(data);
  unsigned int z, s, x, size = zsx_read_value(bytes, 32);

  byte * result = zsx_bytes_buffer;

  int *dico = zsx_new(int, zsx_hsh);
  int *initial = zsx_new(int, zsx_hsh);

  int decoded = 0;
  int last = 0;
  int won = 0;
  while (decoded < size)
  {

  }

  zsx_del(dico);
  zsx_del(initial);

  zsx_del(bytes);

  memcpy(zsx_result_buffer, zsx_bytes_buffer, size);
  zsx_len(zsx_result_buffer) = size;
  zsx_result_buffer[size] = 0;
  return zsx_result_buffer;
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

  zsx_chunk = zsx_big;
  zsx_init
    while (encoded < file_size) {

      //win32 buffered read
      ReadFileEx(hFile, zsx_result_buffer, zsx_chunk, &ol, FIO);
      SleepEx(INFINITE, TRUE);
      encoded += zsx_bytes_read;
      ol.Offset += zsx_bytes_read;
      int prev_len = zsx_bytes_read;

      //zsx encoding
      zsx_len(zsx_result_buffer) = zsx_bytes_read;
      size_t len_zsx = zsx_bytes_read - 1;

      for (int z = 0; prev_len > len_zsx; z++)
      {
        prev_len = len_zsx;
        zsx_encode(zsx_result_buffer);
        len_zsx = zsx_len(zsx_result_buffer);
      }

      //c-like file writing
      fwrite(&len_zsx, sizeof(size_t), 1, f);
      fwrite(zsx_result_buffer, len_zsx, 1, f);
      BackLine();
      printf("%.2f%%", (double)encoded / (double)file_size * 100.0);
    }

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


  zsx_chunk = zsx_big;
  //zsx library has a few set of macros.
  //this one takes care of everything about initialization
  zsx_init

    while (encoded < file_size) {
      ReadFileEx(hFile, zsx_result_buffer, zsx_chunk, &ol, FIO);
      SleepEx(INFINITE, TRUE);
      encoded += zsx_bytes_read;
      ol.Offset += zsx_bytes_read;
	  printf("%d %d", encoded, file_size);

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
