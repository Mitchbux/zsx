#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdlib.h> 
#include <string.h> 
#include <math.h>
#include <stdio.h>

 /* -------- aux stuff ---------- */

typedef uint8_t byte;

void* mem_alloc(size_t item_size, size_t n_item) {
  size_t *x = (size_t*)calloc(1, sizeof(size_t)*2 + n_item * item_size);
  x[0] = item_size;
  x[1] = n_item;
  return x + 2;
}
 
void* mem_extend(void *m, size_t new_n) {
  size_t *x = (size_t*)m - 2;
  x = (size_t*)realloc(x, sizeof(size_t) * 2 + *x * new_n);
  if (new_n > x[1])
    memset((char*)(x + 2) + x[0] * x[1], 0, x[0] * (new_n - x[1]));
  x[1] = new_n;
  return x + 2;
}
 
void _clear(void *m) {
  size_t *x = (size_t*)m - 2;
  memset(m, 0, x[0] * x[1]);
}
 
#define _new(type, n) mem_alloc(sizeof(type), n)
#define _del(m)   { free((size_t*)(m) - 2); m = 0; }
#define _len(m)   *((size_t*)m - 1)
#define _setsize(m, n)  m = (byte*)mem_extend(m, n)
#define _extend(m)  m = (byte*)mem_extend(m, _len(m) * 2);


/*--------- bit reader/writer---------*/


typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} writer_t;

void write_bit(writer_t *p,int v) {
  p->bits+=(v&1)<<(8-(++p->power));
  if(p->power>=8)
  {
     p->data[p->start++]=p->bits;
     p->power=0;
     p->bits=0;
     if(p->start>_len(p->data)-4)
     _extend(p->data)
     
  }
}

void write_value(writer_t *p,unsigned long v, int n) {
    while(n>0)
    {
        n--;
        write_bit(p,(v>>n)&1);
    }
}

void flushWrite(writer_t *p) {
	while(p->power>0)
		write_bit(p, 0);
}

typedef struct {
	int power;
	int bits;
	int start;
	char *data;
} reader_t;

int read_bit(reader_t *r) {
    int v = (r->bits>>(8-(++r->power))&1);
    if(r->power>=8)
    {
        r->power=0;
        r->bits=r->data[r->start++];
    }
    return v;
}

int read_value(reader_t *r, int n) {
    int v=0;
    while(n>0)
    {
       n--;
       v=v<<1;
       v+=read_bit(r);
    }
    return v;
}

writer_t *new_writer(byte *out) {
  writer_t *r = _new(writer_t,1);
  r->power=0;
  r->bits=0;
  r->start=0;
  r->data=(char*)out;
  return r;
}

reader_t *new_reader(byte *in) {
  reader_t *r = _new(reader_t,1);
  r->power=0;
  r->bits=in[0];
  r->start=1;
  r->data=(char*)in;
  return r;
}

/*----- sx related -----*/

int lf(unsigned int n)
{
	int m=1;
	while((unsigned int)(1<<m)<n)m++;
	return m;
}
void sx_write(writer_t*writer,int n,int o)
{
	write_value(writer,n,lf(o));
}
int sx_read(reader_t*reader,int o)
{
	return read_value(reader,lf(o));
}

#define sx_bits 8
#define sx_values 256
#define sx_top 64



int code[sx_values];
int solo[sx_values];

int sorted[sx_top];
int initial[sx_top];

int *list;
char *bytes_buffer;
char *result_buffer;

#define ZSX

/****** zsx ******/
byte *zsx_encode(byte *data,int len) {
	
	
	int s,x,n,k,v,g;
	writer_t *bytes = new_writer(bytes_buffer);
	
	byte *result;
	
	reader_t *reader = new_reader(data);
	//printf("input size: %d\n", len);
	printf(".");
	
	for(s=0; s<sx_values; s++){ code[s]=0; }
	int items = 0;
	int total;

	while( reader->start <= len ) {
		x = read_value( reader, sx_bits );
		code[x]++;
		list[items++]=x;
		}
	
	
	while(items)
	{
		int new_len=items;
		for(s=0;s<sx_values;s++){solo[s]=0;}
	
	for(s=1; s<sx_top; s++){
		int max=code[0],index=0;
		for(k=0; k<sx_values; k++)
		if(code[k] > max) max = code[k],index=k;
		code[index] = -1;
		solo[index] = s;
		write_value(bytes,index,sx_bits);
	}
	
	for(s=0; s<sx_values; s++){ code[s]=0; }
	for(s=0; s<sx_top; s++){ sorted[s]=0; }
	items=0;
	int last = 0;
	int init = 0;
	for(k=0;k<new_len;k++) {
	
		x = list[k];
		s=solo[x];
		if(solo[x]==0)
		{
			list[items++]=x;
			code[x]++;
		}
		if(last>0)
		write_bit(bytes, s>initial[last-1]?1:0);
		if(sorted[s]==0)
		{
			if(last>0)
			if(s>initial[last-1]){
				
				int prev=initial[0];
				write_value(bytes, initial[0],lf(sx_top));
				for(s=1;s<last;s++)
				{
					while(prev-->initial[s])write_bit(bytes,1);
					write_bit(bytes,0);
				}
				for(x=0;x<sx_top;x++)
					sorted[x]=0;
			
				last=0;
			}
			
			initial[last] = s;
			sorted[s] = ++last;
		
		}
		{
			write_value(bytes, sorted[s]-1,lf(last));
		}
		
		
		
	}
				

		

	}
	
	
	flushWrite(bytes);
	*((size_t*)result_buffer - 1)=bytes->start+8;
	result = result_buffer;

	memcpy(result,&len,sizeof(int));
	memcpy(result+sizeof(int),&bytes->start,sizeof(int));
	memcpy(result+sizeof(int)+sizeof(int),bytes->data,bytes->start);
	
	
	return result;
}


int values[sx_values];
byte *zsx_decode(byte *data,int len) {
	unsigned int x,n,s,k,v,size =*(int*)data;

	int stop=*(int*)(data+sizeof(int));
	
	byte *bytes_data = data+sizeof(int)+sizeof(int);
    
	puts("decoding...");
	
	
	byte *result = _new(byte,size);
	
	reader_t *bytes = new_reader(bytes_data);
	
	int **decode = _new(int*,10);
    int *encoded = _new(int, size);	
	int *marked = _new(int, size);
	int *count = _new(int, 10);
	int *dico = _new(int, sx_top);
	int items = 0;
    int index = 0;
	int new_items = size;
	while(new_items>0)
	{
	
		for(s=0;s<sx_values;s++){solo[s]=0;}
		for(s=1;s<sx_top;s++)
		{
			solo[s]=read_value(bytes,sx_bits);
			//printf("%d ", solo[s]);
		}
		//printf("\n");
		items = new_items;
		new_items = 0;
		int decoded = 0;
		decode[index] = _new(int, items);
		count[index] = items;
		int last = 0;
		int start = 0;
		while(decoded<items) {
		
	    int b = read_bit(bytes);
		if(b==1)
		{
			s = last++;
		}else{
			s = read_value(bytes, lf(last));
		}
		encoded[decoded++] = s;
		if(last==0)
		{
			for(s=0;s<sx_top;s++)
			{
				b = read_bit(bytes);
				if(b)
				{
					x = read_value(bytes, lf(last));
					dico[x] = s;
				}
			}
			for(x=start;x<decoded;x++)
			{
				if(dico[encoded[x]]==0)
				{
					new_items++;
					decode[index][x] = -1;
				}else
				{
					s = dico[encoded[x]];
					decode[index][x]=solo[s];
				}
				
			}
					
			start = decoded;
		}

		}
		for (s = 0; s < last; s++)
		{
			dico[s] = read_value(bytes, lf(sx_top));
		}
		for(x=start;x<decoded;x++)
		{
				if(dico[encoded[x]]==0)
				{
					new_items++;
					decode[index][x] = -1;
				}else
					decode[index][x]=solo[dico[encoded[x]]];			
		}
		index++;
		
	}
	
	int indexes = index;
	index-=2;
	for(s=0;index>=0;index--)
	{
		for(n=0,s=0;s<count[index];s++)
		{	
			if(decode[index][s]==-1)
			{
				decode[index][s] = decode[index+1][n++];
			}
		}
	}
	
	for(x=0;x<count[0];x++)
	{
		result[x] = decode[0][x];

	}	
	
	for(s=0;s<indexes;s++)
		_del(decode[s])
	_del(decode)
		printf("ok");
	return result;
}


int test() {
	
  int i,fd = open("enwik9", O_RDONLY);
  if (fd == -1) {
    printf( "Can't read file!\n");
	return 0;
  };

  struct stat st;
  fstat(fd, &st);
  
 int chunk = 50*1000*1000;
  
  int encoded=0;
  byte *in = (byte*)_new(char, chunk);
  bytes_buffer=_new(char,chunk*2);
  result_buffer=_new(char,chunk*2);
  list = _new(int, chunk);
  
  FILE* f=fopen("enwik9.zsx","wb");
  
  if (f==NULL) {
    printf( "Can't create file!\n");
	return 0;
  };
  printf("encoding...\n");
  
while(encoded<st.st_size)
  {
  int bytes_read=read(fd, in, chunk);
 encoded+=bytes_read;
  
  
  
  int prev_len=bytes_read;
  byte *data	= zsx_encode(in,bytes_read);
 // printf("dictionary: %ld.",_len(data));
 
#ifdef ZSX 

  /*
  byte *zsx    = _new(byte,_len(in)*14);
  z_stream defstream;
    defstream.zalloc = Z_NULL;
    defstream.zfree = Z_NULL;
    defstream.opaque = Z_NULL;
    
    defstream.avail_in = _len(data);
    defstream.next_in = data;
    defstream.avail_out = _len(zsx);
    defstream.next_out = zsx; 

    deflateInit(&defstream, Z_BEST_COMPRESSION);
    deflate(&defstream, Z_FINISH);
    deflateEnd(&defstream);
  
  size_t len_zsx=defstream.next_out-zsx;
  	*/
// printf("compressed: %zu\n",len_zsx);
  
 size_t len_zsx =_len(data);
 
  while(len_zsx<(prev_len)) {
  	prev_len=len_zsx;
  	//_del(data)
    data	= zsx_encode(data,len_zsx);

  
    len_zsx =_len(data);
  }
  
  //printf("final size: %ld.\n",len_zsx);
  
  fwrite(&len_zsx,sizeof(size_t),1,f);
  fwrite(data,len_zsx,1,f);
  
  printf("%.2f%%.\n",(double)encoded/(double)st.st_size*100.0);
  
  
  #else
  
  
byte *decoded=zsx_decode(data,_len(data));
fwrite(decoded,_len(decoded),1,f);
puts(decoded);
  printf("%.2f%%.\n",(double)encoded/(double)st.st_size*100.0);

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
