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
#define zsx_extend(m) m = (byte *)zsx_extend(m, zsx_len(m) * 2);

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
		if(p->canextend&&((zsx_len(p->data))-p->start)<sizeof(int))
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

#define zsx_chunk_bits 30
#define zsx_chunk (1<<zsx_chunk_bits)
#define zsx_bits 8
#define zsx_half 4
#define zsx_values 256
#define zsx_items 3
#define zsx_top 256*256

char *zsx_bytes_buffer;
char *zsx_result_buffer;

#define zsx_init zsx_bytes_buffer = zsx_new(char, zsx_chunk); zsx_result_buffer = zsx_new(char, zsx_chunk);

#define ZSX


/****** zsx ******/
byte *zsx_encode(byte *data) {
	//printf(".");
	int len = zsx_len(data);
	byte *result;
	zsx_writer_t *bytes = zsx_writer(zsx_bytes_buffer, 0);
	zsx_reader_t *reader = zsx_reader(data);
	zsx_write_value(bytes, len, 32);
	int *initial = zsx_new(int, zsx_top);
	int *indexes = zsx_new(int, zsx_top);
	int *idx = zsx_new(int, zsx_top);
	int id , in = 0, next = 0, last = 0, count = 0;
	
	for(int s=0;s<zsx_top;s++)
		idx[s]=0, indexes[s]=0;
	
	while (in < len ) {
    int z = data[in++] , s = data[in++], x = data[in++], hash = (z<<zsx_bits) ^(s << zsx_half) ^ x;
		
			

					
				zsx_write_bit(bytes, indexes[hash]);
				if(indexes[hash]) 
				{
					zsx_write_value(bytes, indexes[hash], zsx_bits);
					if(idx[indexes[hash]])
					{
						zsx_write_value(bytes, hash, zsx_bits+zsx_bits);
						idx[indexes[hash]]=0;
					}
					indexes[hash] = 0;
				}else
				{
					indexes[hash]=s;
					if(idx[s])
					{
						zsx_write_value(bytes, hash, zsx_bits+zsx_bits);
						zsx_write_value(bytes, idx[s] - 1, zsx_bits+zsx_bits);
						indexes[idx[s]-1] = 0;
						idx[s] = 0;
					}else
						idx[s]=hash+1;
					
				}

				zsx_write_bit(bytes, initial[hash] != s ? 1 : 0);
				if (initial[hash] != s){ initial[hash] = s;
					zsx_write_value(bytes, s, zsx_bits);}		

	}
	
	for(int s=0;s<zsx_values;s++)
		if(idx[s])
		{
			zsx_write_value(bytes, idx[s]-1, zsx_bits+zsx_bits);
		}
						
	
	zsx_flushWrite(bytes);
	zsx_len(zsx_result_buffer) = bytes->start;
	result = zsx_result_buffer;
	memcpy(result, bytes->data, bytes->start);

	zsx_del(initial);
	zsx_del(indexes);
	zsx_del(idx);
	
	
	zsx_del(reader);
	zsx_del(bytes);
	return zsx_result_buffer;
}

byte *zsx_decode(byte *data) {
	zsx_reader_t *bytes = zsx_reader(data);
	unsigned int z, s, x, c, b, size = zsx_read_value(bytes, 32);

	byte * result = zsx_bytes_buffer;

	int *dico = zsx_new(int, zsx_values);
	
	int decoded = 0;
	int last = 0;


	while (decoded < size)
	{
		if (zsx_read_bit(bytes))
		{
		}
		else
		{
		}
	}

	zsx_del(dico)	
	zsx_del(bytes);

	memcpy(zsx_result_buffer, zsx_bytes_buffer, size);
	zsx_len(zsx_result_buffer) = size;
	return zsx_result_buffer;
}

int test(char * filename) {

  /*** Encoding file chunk by chunk ***/

  //c-like unix friendly file handling
  
	int i, fd = open(filename, O_RDONLY);
	if (fd == -1) {
		printf("Can't read file!\n");
		return 0;
	};

	struct stat st;
	fstat(fd, &st);

	int encoded = 0;
  char outfilename[256];
  strcpy(outfilename, filename);
  strcat(outfilename, ".tmp");
  
	FILE *f = fopen(outfilename, "wb");
	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};

	printf("\nEncoding...\n");

  zsx_init
  
	while (encoded < st.st_size) {
	 
    //unix read file buffer
    int bytes_read = read(fd, zsx_result_buffer, zsx_chunk);
		encoded += bytes_read;
    int prev_len = bytes_read;
    
    //zsx encode chunk
    zsx_len(zsx_result_buffer) = bytes_read;
		byte *data = zsx_encode(zsx_result_buffer);
		size_t len_zsx = zsx_len(data);

    //as much as it can be encoded
		while (len_zsx < (prev_len)) {
			prev_len = len_zsx;
			data = zsx_encode(data);
			len_zsx = zsx_len(data);
		}

    //unix write buffer to file
		fwrite(&len_zsx, sizeof(size_t), 1, f);
		fwrite(data, len_zsx, 1, f);

		printf("\r%.2f%%\n\033[A", (double)encoded / (double)st.st_size * 100.0);

  }
	fclose(f);
	close(fd);

  //*** re-encoding the whole file to a minimum set of data ***

	//unix file handling
	fd = open(outfilename, O_RDONLY);
	if (fd == -1) {
		printf("Can't read file!\n");
		return 0;
	};

	fstat(fd, &st);
	
  encoded = 0;
  strcpy(outfilename, filename);
  strcat(outfilename, ".zsx");
  
	f = fopen(outfilename, "wb");
	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};


	printf("\nFinalizing...\n");

	#undef zsx_chunk_bits
	#define zsx_chunk_bits 26
	zsx_init

	while (encoded < st.st_size) {
	 
    //unix read file buffer
    int bytes_read = read(fd, zsx_result_buffer, zsx_chunk);
		encoded += bytes_read;
    int prev_len = bytes_read;
 
		//zsx encoding
		zsx_len(zsx_result_buffer) = bytes_read;
		byte *data = zsx_encode(zsx_result_buffer);
		size_t len_zsx = zsx_len(data);

		//as much as it can
		while (prev_len > len_zsx)
		{
			prev_len = len_zsx;
			data = zsx_encode(zsx_result_buffer);
			len_zsx = zsx_len(data);
		}

		//and write it c-like
		fwrite(&len_zsx, sizeof(size_t), 1, f);
		fwrite(data, len_zsx, 1, f);
	}

	fclose(f);
	close(fd);

	printf("Compressed to %s.\n", outfilename);
	
	//TODO : delete tmp file and deal with memory
	
	return 1;
}

int testdec(char *filename) {

	//*** Testing Encoding and Decoding ***

	//unix file handling
	int i, fd = open(filename, O_RDONLY);
	if (fd == -1) {
		printf("Can't read file!\n");
		return 0;
	};

	struct stat st;
	fstat(fd, &st);

	int encoded = 0;
  char outfilename[256];
  strcpy(outfilename, filename);
  strcat(outfilename, ".tmp");
  
	FILE *f = fopen(outfilename, "wb");
	if (f == NULL) {
		printf("Can't create file!\n");
		return 0;
	};

	printf("\nTesting...\n");


	//zsx library has a few set of macros.
	//this one takes care of everything about initialization
	zsx_init
	
	while (encoded < st.st_size) {
	 
    //unix read file buffer
    int bytes_read = read(fd, zsx_result_buffer, zsx_chunk);
		encoded += bytes_read;
    int prev_len = bytes_read;

	printf("\nEncoding twice...\n");
	
		//zsx_len is a dereferenced pointer can be used as left-op or right-op and describes the size of allocated memory 
		zsx_len(zsx_result_buffer) = bytes_read;
		byte * data = zsx_encode(zsx_result_buffer);		
		data = zsx_encode(data);

	printf("\nDecoding twice...\n");
	
		byte *decompressed = zsx_decode(data);
		decompressed = zsx_decode(decompressed);
		
		puts(decompressed);
		//zsx_len contains the size of the data but not what was actually allocated at first. 
		// !! Caution !! do not use these macro for any other context than compression.
		fwrite(decompressed, zsx_len(decompressed), 1, f);
	}

	fclose(f);
	close(fd);
	
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