/* Portable ZSX1 file compressor/decompressor. */
#ifndef _WIN32
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#define ZSX_STAT_STRUCT struct _stat64
#define zsx_stat _stat64
#define zsx_isreg(m) (((m) & _S_IFMT) == _S_IFREG)
#define zsx_open _open
#define zsx_fdopen _fdopen
#define zsx_close _close
#define zsx_getpid _getpid
#else
#include <unistd.h>
#define ZSX_STAT_STRUCT struct stat
#define zsx_stat stat
#define zsx_isreg(m) S_ISREG(m)
#define zsx_open open
#define zsx_fdopen fdopen
#define zsx_close close
#define zsx_getpid getpid
#endif

// .:: / auxilary memory routines /::.
// ...................................

typedef uint8_t byte;
typedef uint16_t ushort;

void *zsx_alloc(size_t item_size, size_t n_item) {
	if (item_size && n_item > (SIZE_MAX - sizeof(size_t) * 2) / item_size) {
		fprintf(stderr, "zsx: allocation size overflow\n");
		exit(1);
	}
	size_t *x = (size_t *)calloc(1, sizeof(size_t) * 2 + n_item * item_size);
	if (!x) {
		fprintf(stderr, "zsx: out of memory\n");
		exit(1);
	}
	x[0] = item_size;
	x[1] = n_item;
	return x + 2;
}

void zsx_clear(void *m) {
	size_t *x = (size_t *)m - 2;
	memset(m, 0, x[0] * x[1]);
}

#define zsx_new(type, n) zsx_alloc(sizeof(type), n)
#define zsx_del(m) free((size_t *)(m)-2)
#define zsx_len(m) *((size_t *)m - 1)

// .:: / binary range coder /::.
// A flat bit costs one bit whatever it says. These do not : every bit is
// coded against a probability that follows how that same bit behaved before,
// so a bit that is nearly always zero ends up costing nearly nothing.
// The range is 32 bits, the low is 64 so a carry has room to show up, and one
// cached byte plus a run length carries it back through the pending 0xFFs.
// .................................

//probabilities live on 11 bits and move by a sixteenth of the distance left
#define zsx_prob_bits 0xB
#define zsx_prob_one (1<<zsx_prob_bits)
#define zsx_prob_half (zsx_prob_one>>1)
#ifndef zsx_prob_move
#define zsx_prob_move 4
#endif
//under this the range no longer splits finely enough, so we push a byte out
#define zsx_top (1<<0x18)

typedef struct {
	uint64_t low;
	unsigned int range;
	byte cache;
	int64_t pending;
	byte *data;
	int start;
} zsx_coder_t;

typedef struct {
	unsigned int range;
	unsigned int code;
	byte *data;
	int start;
	int ends;
	int failed;
} zsx_decoder_t;

//a fresh table of probabilities, all of them undecided
ushort *zsx_probs(int n) {
	ushort *p = zsx_new(ushort, n);
	for(int i=0;i<n;i++)
		p[i] = zsx_prob_half;
	return p;
}

zsx_coder_t *zsx_coder(byte *out) {
	zsx_coder_t *c = zsx_new(zsx_coder_t, 1);
	c->low = 0;
	c->range = 0xFFFFFFFF;
	c->cache = 0;
	//the first shift only primes the cache, it must not emit a byte
	c->pending = 1;
	c->data = out;
	c->start = 0;
	return c;
}

//pushes the top byte of the low out. While that byte is 0xFF it can still be
//turned into 0x00 by a later carry, so we only count it and settle the whole
//run once a byte that can absorb the carry shows up
void zsx_carry(zsx_coder_t *c) {
	if((unsigned)(c->low>>0x20) || (unsigned)c->low < 0xFF000000u)
	{
		byte t = c->cache;
		do {
			c->data[c->start++] = t + (byte)(c->low>>0x20);
			t = 0xFF;
		} while(--c->pending);
		c->cache = (byte)((unsigned)c->low>>0x18);
	}
	c->pending++;
	c->low = (unsigned)((unsigned)c->low << 8);
}

//a probability walks a fifth of the way towards what it just saw. It never
//reaches 0 or 1 : it has to keep paying for a surprise, and 0 is left free
//to mean a probability nobody has touched yet
void zsx_update(ushort *prob, int bit) {
	if(!bit)
		*prob += (zsx_prob_one - *prob)>>zsx_prob_move;
	else
		*prob -= *prob>>zsx_prob_move;
}

void zsx_code_bit(zsx_coder_t *c, ushort *prob, int bit) {
	//the range is cut where the probability says, the bit picks a side
	unsigned int bound = (c->range>>zsx_prob_bits) * *prob;
	if(!bit)
		c->range = bound;
	else
		c->low += bound, c->range -= bound;
	zsx_update(prob, bit);
	while(c->range < zsx_top)
		c->range <<= 8, zsx_carry(c);
}

//bits nothing is known about, they cost exactly what they say
void zsx_code_direct(zsx_coder_t *c, unsigned int value, int n) {
	while(n--)
	{
		c->range >>= 1;
		c->low += c->range & (0u - ((value>>n)&1));
		while(c->range < zsx_top)
			c->range <<= 8, zsx_carry(c);
	}
}

// .::/ zsx_code_byte /::.
// Codes a byte as eight bits down a tree of probabilities : the node we sit on
// spells the bits already sent, so every bit is coded knowing the ones above it.
// Two trees stand behind each other, the wide context in front of the narrow
// one. Only the wide one codes ; the narrow one just keeps learning. A node of
// the wide tree we have never been to is handed what the narrow one already
// knows, so a context met for the first time never costs more than a shallow one
void zsx_code_byte(zsx_coder_t *c, ushort *deep, ushort *wide, int value) {
	int node = 1;
	for(int i=7;i>=0;i--)
	{
		int bit = (value>>i)&1;
		if(!deep[node]) deep[node] = wide[node];
		zsx_code_bit(c, deep+node, bit);
		zsx_update(wide+node, bit);
		node = (node<<1) + bit;
	}
}

//the low still holds five bytes worth of the final interval
void zsx_flush(zsx_coder_t *c) {
	for(int i=0;i<5;i++)
		zsx_carry(c);
}

//past the end of the stream we feed zeroes rather than read out of the buffer
unsigned int zsx_next(zsx_decoder_t *d) {
	if (d->start < d->ends)
		return (byte)d->data[d->start++];
	d->failed = 1;
	return 0;
}

zsx_decoder_t *zsx_decoder(byte *in, int ends) {
	zsx_decoder_t *d = zsx_new(zsx_decoder_t, 1);
	d->range = 0xFFFFFFFF;
	d->code = 0;
	d->data = in;
	//the first byte the coder emits is the primed cache, it carries nothing
	d->start = 1;
	d->ends = ends;
	d->failed = ends < 5;
	for(int i=0;i<4;i++)
		d->code = (d->code<<8) | zsx_next(d);
	return d;
}

int zsx_decode_bit(zsx_decoder_t *d, ushort *prob) {
	unsigned int bound = (d->range>>zsx_prob_bits) * *prob;
	int bit;
	//which side of the cut the code landed on is the bit that was sent
	if(d->code < bound)
		d->range = bound, bit = 0;
	else
		d->code -= bound, d->range -= bound, bit = 1;
	zsx_update(prob, bit);
	while(d->range < zsx_top)
		d->range <<= 8, d->code = (d->code<<8) | zsx_next(d);
	return bit;
}

unsigned int zsx_decode_direct(zsx_decoder_t *d, int n) {
	unsigned int value = 0;
	while(n--)
	{
		d->range >>= 1;
		unsigned int bit = 0;
		if(d->code >= d->range)
			d->code -= d->range, bit = 1;
		value = (value<<1) | bit;
		while(d->range < zsx_top)
			d->range <<= 8, d->code = (d->code<<8) | zsx_next(d);
	}
	return value;
}

int zsx_decode_byte(zsx_decoder_t *d, ushort *deep, ushort *wide) {
	int node = 1;
	while(node < 0x100)
	{
		if(!deep[node]) deep[node] = wide[node];
		int bit = zsx_decode_bit(d, deep+node);
		zsx_update(wide+node, bit);
		node = (node<<1) + bit;
	}
	return node - 0x100;
}


//buffer size : 33Mb
#define zsx_buffer 0x2000000
//chunk size : 12Mb
#define zsx_chunk 0xC00000
#define zsx_word 0x10000
#define zsx_byte 0x100
//the header is : original length, stream length, literal count, crc
#define header_sizes (sizeof(int)*4)

//.:: how much context each coded decision gets ::.
//a wider context says more about what comes next, but it also splits the data
//thinner : probabilities that never see enough of it stay undecided and the
//bits they code keep costing full price. These are the two knobs.
#ifndef zsx_lc
//bits of the last token that pick the probabilities of a spelled out token
#define zsx_lc 8
#endif
#ifndef zsx_hc
//bits of the token before it that join the last one for the verdict
#define zsx_hc 8
#endif
#ifndef zsx_dc
//bits of the two last tokens that pick the tree a spelled out token is coded
//down. The last token is kept whole and the one before it is cut down to what
//is left : it is the weaker hint, so it is the one that gives ground
#define zsx_dc 0x10
#endif
#ifndef zsx_mc
//bits the three last tokens are folded into. This context only remembers one
//token, not a whole tree of probabilities, so it can be kept wide for almost
//nothing : two bytes a context instead of half a kilobyte
#define zsx_mc 0x18
#endif
#ifndef zsx_qc
//and the same again for the four last tokens. Each level added in front only
//ever costs where it has something to say : where it has not been written it
//is skipped without a bit, so the levels stack without paying for each other
#define zsx_qc 0x18
#endif

#ifndef zsx_rank_min
//how often a two byte word has to show up before it is worth naming in the
//rank table it costs 16 bits to name it in
#define zsx_rank_min 0xA
#endif

#define zsx_lit_size ((1<<zsx_lc)*zsx_byte)
#define zsx_hit_size ((1<<zsx_hc)*zsx_byte)
#define zsx_deep_size ((1<<zsx_dc)*zsx_byte)
#define zsx_lit_ctx(x) (((x)>>(8-zsx_lc))<<8)
#define zsx_hit_ctx(s,x) ((((s)>>(8-zsx_hc))<<8) + (x))
#ifndef zsx_deep_ctx
#define zsx_deep_ctx(s,x) (((((s)>>(0x10-zsx_dc))<<8)+(x))<<8)
#endif
#ifndef zsx_most_ctx
#define zsx_most_ctx(r,s,x) \
	((((unsigned)(((r)<<0x10)+((s)<<8)+(x)))*0x9E3779B1u)>>(0x20-zsx_mc))
#endif
#define zsx_most_size (1<<zsx_mc)
#ifndef zsx_vast_ctx
#define zsx_vast_ctx(q,r,s,x) \
	((((unsigned)(((q)<<0x18)+((r)<<0x10)+((s)<<8)+(x)))*0x85EBCA77u)>>(0x20-zsx_qc))
#endif
#define zsx_vast_size (1<<zsx_qc)


//sorts the word ids by descending count, hoare partition.
//the pivot value is always one of the elements, so it acts as a sentinel
//and both scans stay inside the array.
void zsx_quicksort_count(int *list, int *count,int len) {
	while(len>1){
		int s,x,t;
		int pivot = count[list[len/2]];
		for(s=0,x=len-1;s<=x;){
			while(count[list[s]]>pivot)s++;
			while(count[list[x]]<pivot)x--;

			if(s>x)break;

			t=list[s], list[s]=list[x], list[x]=t;
			s++, x--;
		}

		//recurse on the small side, iterate on the big one to bound the stack
		if(x+1 < len-s)
			zsx_quicksort_count(list, count, x+1),
			list+=s, len-=s;
		else
			zsx_quicksort_count(list+s, count, len-s),
			len=x+1;
	}
}

int zsx_predict(byte *data, zsx_coder_t *rc) {
	int s, x, r;
	int len = zsx_len(data);
	int *value = zsx_new(int, zsx_word);
	int *count = zsx_new(int, zsx_word);
	int *index = zsx_new(int, zsx_word);
	byte *ends = data + len;
	int literals = 0;

	zsx_clear(count);
	for (int at = 0; at + 1 < len; at += 2)
		count[data[at] | ((unsigned)data[at + 1] << 8)]++;

	for(s=0;s<zsx_word;s++)
		value[s]=s;

	zsx_quicksort_count(value, count, zsx_word);

	//a ranked word costs a flat 16 bits to name, whatever it is worth. One that
	//shows up too rarely never earns that back, so the table stops where the
	//counts do : on a small or a random file it can end up empty
	int ranked = 0;
	while(ranked < zsx_byte-1 && count[value[ranked]] >= zsx_rank_min)
		ranked++;

	zsx_code_direct(rc, ranked, 8);
	for(s=1;s<=ranked;s++)
		zsx_code_direct(rc, value[s-1],0x10),
		index[value[s-1]]=s;

	zsx_del(count);
	zsx_del(value);

	ushort **zsx = zsx_new(ushort*, zsx_byte);
	for(s=0;s<zsx_byte;s++)
		zsx[s]=zsx_new(ushort, zsx_byte);

	//the things we send, each with its own probabilities :
	//whether the token was a word, told by the last token and the last verdict,
	//whether the order 2 context guessed right, told by that same context,
	//whether the order 1 guess saves us when it did not, told by the last token,
	//and the token itself when neither did, told by the last token too.
	//hint is the order 2 verdict under the last token alone : a wide context we
	//have never met starts from what the narrow one already learned instead
	//of starting undecided, so meeting it costs nothing extra
	ushort *pack = zsx_probs(zsx_byte*2);
	ushort *hits = zsx_new(ushort, zsx_hit_size);
	ushort *hint = zsx_probs(zsx_byte);
	ushort *miss = zsx_probs(zsx_lit_size);
	ushort *deep = zsx_new(ushort, zsx_deep_size);
	//the order 3 guess and its own verdict, asked first because it is the one
	//that is right most often. It remembers a single token a context, so three
	//tokens worth of context fit in a few megabytes instead of a few gigabytes.
	//it holds the token plus one, so zero still means never written
	ushort *most = zsx_new(ushort, zsx_most_size);
	ushort *tops = zsx_new(ushort, zsx_hit_size);
	ushort *tip = zsx_probs(zsx_byte);
	int was = 0;

	r=0;
	s=0;
	x=0;
	while(data<ends)
	{
		byte isindex = 0;
		register byte first = *data++;
		if(data<ends)
		{
			register ushort word = first + (*data<<8);
			if(index[word])
				first = index[word], isindex=1, data++;
		}

		zsx_code_bit(rc, pack + x*2 + was, isindex);
		was = isindex;

		//the deepest guess speaks first : when it is right, and on text it
		//usually is, the token has cost one nearly free bit and nothing else.
		//a context nothing was ever written under has nothing to say, and both
		//sides know it, so no bit is spent asking
		unsigned int h3 = zsx_most_ctx(r,s,x);
		int top = 0;
		if(most[h3])
		{
			top = most[h3]-1==first;
			ushort *deepest = tops + zsx_hit_ctx(s,x);
			if(!*deepest) *deepest = tip[x];
			zsx_code_bit(rc, deepest, top);
			zsx_update(tip+x, top);
		}

		if(!top)
		{
			//a hit costs almost nothing where the context is reliable, and
			//a context that has never predicted anything is skipped the same
			//way the order 3 one is
			int hit = 0;
			if(zsx[s][x])
			{
				hit = zsx[s][x]-1==first;
				ushort *verdict = hits + zsx_hit_ctx(s,x);
				if(!*verdict) *verdict = hint[x];
				zsx_code_bit(rc, verdict, hit);
				zsx_update(hint+x, hit);
			}

			if(!hit)
			{
				zsx_code_byte(rc, deep + zsx_deep_ctx(s,x), miss + zsx_lit_ctx(x), first);
				literals++;
			}
		}

		//both guesses are kept on the token that actually came
		zsx[s][x] = first+1;
		most[h3] = first+1;

		r=s;
		s=x;
		x=first;
	}

	for(s=0;s<zsx_byte;s++)
		zsx_del(zsx[s]);
	zsx_del(zsx);
	zsx_del(index);
	zsx_del(pack);
	zsx_del(hits);
	zsx_del(hint);
	zsx_del(miss);
	zsx_del(deep);
	zsx_del(most);
	zsx_del(tops);
	zsx_del(tip);

	return literals;
}

// .::/ zsx_encode /::.
// Compress a byte buffer using zsx_predict.
// .......................
// byte * data 			: the data to compress
// .......................
// returns the compressed byte* buffer
byte *zsx_encode(byte *data) {
	size_t length = zsx_len(data);

	//allocate buffers
	byte *buffer = zsx_new(byte, zsx_buffer);
	zsx_coder_t *rc = zsx_coder(buffer);

	//zsx_predict returns how many tokens the context failed to guess, and
	//leaves one single coded stream behind it
	int bytes_len = zsx_predict(data, rc);
	zsx_flush(rc);
	int first_len = rc->start;

	//data nothing can be said about costs more to code than to copy. When that
	//happens we keep the copy, so a chunk never grows by more than its header.
	//a miss count of -1 is the mark : zsx_predict can never return it
	if(first_len >= (int)length)
	{
		memmove(data + header_sizes, data, length);
		first_len = length;
		bytes_len = -1;
	}
	else
		//the stream goes right behind the header
		memcpy(data + header_sizes, buffer, first_len);

	//TODO : CRC Check
	int crc = 0;

	//set size header
	data[0] = (byte)length;
	data[1] = (byte)(length >> 8);
	data[2] = (byte)(length >> 16);
	data[3] = (byte)(length >> 24);
	data[4] = (byte)first_len;
	data[5] = (byte)(first_len >> 8);
	data[6] = (byte)(first_len >> 16);
	data[7] = (byte)(first_len >> 24);
	data[8] = (byte)bytes_len;
	data[9] = (byte)(bytes_len >> 8);
	data[10] = (byte)(bytes_len >> 16);
	data[11] = (byte)(bytes_len >> 24);
	data[12] = (byte)crc;
	data[13] = data[14] = data[15] = 0;

	zsx_len(data) = header_sizes + first_len;

	//free the buffers and the range coder
	zsx_del(rc);
	zsx_del(buffer);
	return data;
}

// .::/ zsx_predict_read /::.
// Decode the predict algorithm to a byte buffer. Exact inverse of zsx_predict :
// the same probabilities are grown from the same decisions in the same order,
// so both sides always cut the range at the same place.
// .......................
// byte * stream	 	: the whole compressed payload
// byte * result	    : the decoded data buffer
// int len				: the size of the initial data
// int stream_len		: how many coded bytes the stream holds
// int bytes_len		: how many tokens the context failed to guess
// .......................
// returns the decoded byte * result buffer, 0 if failed

byte * zsx_predict_read(byte *stream, byte*result, int len, int stream_len, int bytes_len)
{
	int z, s, x, r;
	int fail = 0;
	int decoded = 0, missed = 0;

	//rank -> word, the reverse of the encoder's word -> rank table
	int *index = zsx_new(int, zsx_byte);
	zsx_decoder_t *rc = zsx_decoder(stream, stream_len);

	//we use the predicted array to store and reload predicates
	ushort **zsx = zsx_new(ushort*, zsx_byte);
	for(s=0;s<zsx_byte;s++)
		zsx[s]=zsx_new(ushort, zsx_byte);

	//the same probability tables the encoder grew, and the same rule that
	//hands a never seen context what the last token alone already knows
	ushort *pack = zsx_probs(zsx_byte*2);
	ushort *hits = zsx_new(ushort, zsx_hit_size);
	ushort *hint = zsx_probs(zsx_byte);
	ushort *miss = zsx_probs(zsx_lit_size);
	ushort *deep = zsx_new(ushort, zsx_deep_size);
	ushort *most = zsx_new(ushort, zsx_most_size);
	ushort *tops = zsx_new(ushort, zsx_hit_size);
	ushort *tip = zsx_probs(zsx_byte);
	int was = 0;

	//we read the word dictionary (rank -> word) first, since the tokens
	//below are transmitted as ranks and need this mapping to decode. Its
	//length comes first : the encoder only kept the words that paid for
	//themselves, and on some files it kept none
	int ranked = zsx_decode_direct(rc, 8);
	for(s = 1; s <= ranked; s++)
		index[s] = zsx_decode_direct(rc, 0x10);

	r = 0;
	s = 0;
	x = 0;
	while(!fail && decoded < len) {

		//we need to know if the token is a word index
		int is = zsx_decode_bit(rc, pack + x*2 + was);
		was = is;

		//the order 3 guess is asked first, exactly as it was sent : a context
		//nothing was ever written under is silent on both sides
		unsigned int h3 = zsx_most_ctx(r,s,x);
		int top = 0;
		if(most[h3])
		{
			ushort *deepest = tops + zsx_hit_ctx(s,x);
			if(!*deepest) *deepest = tip[x];
			top = zsx_decode_bit(rc, deepest);
			zsx_update(tip+x, top);
		}

		//if the deepest context called it we reload what it remembers
		if(top)
			z = most[h3]-1;
		else
		{
			int hit = 0;
			if(zsx[s][x])
			{
				ushort *verdict = hits + zsx_hit_ctx(s,x);
				if(!*verdict) *verdict = hint[x];
				hit = zsx_decode_bit(rc, verdict);
				zsx_update(hint+x, hit);
			}

			//if it's predicted we reload it
			if(hit)
				z = zsx[s][x]-1;
			//it was spelled out otherwise
			else
				z = zsx_decode_byte(rc, deep + zsx_deep_ctx(s,x), miss + zsx_lit_ctx(x)), missed++;
		}

		//both guesses are kept on the token that actually came
		zsx[s][x] = z+1;
		most[h3] = z+1;

		//an index expands back to the two bytes of the word, low byte first
		if(is)
		{
			if(z <= 0 || z > ranked || decoded+2 > len) {fail = 1; break;}
			result[decoded++] = index[z];
			result[decoded++] = index[z]>>8;
		}else
			result[decoded++] = z;

		//we save the two last tokens for prediction
		r=s;
		s=x;
		x=z;
	}

	int decoder_ok = !rc->failed && rc->start == rc->ends && rc->code == 0;
	zsx_del(rc);
	zsx_del(index);
	zsx_del(pack);
	zsx_del(hits);
	zsx_del(hint);
	zsx_del(miss);
	zsx_del(deep);
	zsx_del(most);
	zsx_del(tops);
	zsx_del(tip);
	for(s=0;s<zsx_byte;s++)
		zsx_del(zsx[s]);
	zsx_del(zsx);

	//a stream that decoded the wrong length, or missed a different number of
	//times than the header says, did not come out of zsx_predict
	if(fail || !decoder_ok || decoded != len || missed != bytes_len) return 0;

	zsx_len(result) = len;
	result[len] = 0;

	return result;
}

byte *zsx_decode(byte *data);

static int write_all(FILE *file, const void *data, size_t length) {
	return length == 0 || fwrite(data, 1, length, file) == length;
}

static int read_all(FILE *file, void *data, size_t length) {
	return length == 0 || fread(data, 1, length, file) == length;
}

static void put_u32(byte out[4], uint32_t value) {
	out[0] = (byte)value;
	out[1] = (byte)(value >> 8);
	out[2] = (byte)(value >> 16);
	out[3] = (byte)(value >> 24);
}

static uint32_t get_u32(const byte in[4]) {
	return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
		((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static int paths_alias(const char *input, const char *output) {
	ZSX_STAT_STRUCT source, target;
	if (zsx_stat(input, &source) != 0 || zsx_stat(output, &target) != 0)
		return 0;
	return source.st_dev == target.st_dev && source.st_ino == target.st_ino;
}

/* The exclusive creation prevents us from ever writing an existing target. */
static FILE *create_temp_output(const char *output, char **temp_path) {
	static unsigned int sequence;
	size_t output_length = strlen(output);
	for (unsigned int attempt = 0; attempt < 1000; attempt++) {
		char suffix[64];
		unsigned int id = ++sequence;
		snprintf(suffix, sizeof(suffix), ".zsx-tmp-%lu-%u",
			(unsigned long)zsx_getpid(), id);
		size_t total = output_length + strlen(suffix) + 1;
		char *name = (char *)malloc(total);
		if (!name) return 0;
		memcpy(name, output, output_length);
		strcpy(name + output_length, suffix);
#ifdef _WIN32
		int fd = zsx_open(name, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY,
			_S_IREAD | _S_IWRITE);
#else
		int fd = zsx_open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
#endif
		if (fd >= 0) {
			FILE *file = zsx_fdopen(fd, "wb");
			if (!file) {
				zsx_close(fd);
				remove(name);
				free(name);
				return 0;
			}
			*temp_path = name;
			return file;
		}
		free(name);
		if (errno != EEXIST) return 0;
	}
	errno = EEXIST;
	return 0;
}

static int replace_output(const char *temporary, const char *output) {
#ifdef _WIN32
	return MoveFileExA(temporary, output,
		MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return rename(temporary, output) == 0;
#endif
}

static int compress_file(const char *input_path, const char *output_path) {
	ZSX_STAT_STRUCT info;
	if (zsx_stat(input_path, &info) != 0) {
		fprintf(stderr, "zsx: cannot stat '%s': %s\n", input_path, strerror(errno));
		return 0;
	}
	if (!zsx_isreg(info.st_mode)) {
		fprintf(stderr, "zsx: input is not a regular file: '%s'\n", input_path);
		return 0;
	}
	if (paths_alias(input_path, output_path)) {
		fprintf(stderr, "zsx: input and output refer to the same file\n");
		return 0;
	}
	if (info.st_size < 0 || (uint64_t)info.st_size > UINT32_MAX) {
		fprintf(stderr, "zsx: input exceeds the ZSX1 32-bit length limit\n");
		return 0;
	}

	FILE *input = fopen(input_path, "rb");
	if (!input) {
		fprintf(stderr, "zsx: cannot open '%s': %s\n", input_path, strerror(errno));
		return 0;
	}
	char *temporary = 0;
	FILE *output = create_temp_output(output_path, &temporary);
	if (!output) {
		fprintf(stderr, "zsx: cannot create temporary output for '%s': %s\n", output_path, strerror(errno));
		fclose(input);
		return 0;
	}

	uint32_t original = (uint32_t)info.st_size;
	uint32_t chunks = original == 0 ? 0 : (uint32_t)(((uint64_t)original + zsx_chunk - 1) / zsx_chunk);
	byte header[16] = {'Z', 'S', 'X', '1'};
	put_u32(header + 4, original);
	put_u32(header + 8, chunks);
	put_u32(header + 12, 0);
	int ok = write_all(output, header, sizeof(header));

	for (uint32_t i = 0; ok && i < chunks; i++) {
		uint32_t wanted = original - i * (uint32_t)zsx_chunk;
		if (wanted > zsx_chunk) wanted = zsx_chunk;
		byte *chunk = zsx_new(byte, (size_t)zsx_chunk + header_sizes + 1);
		if (!read_all(input, chunk, wanted)) {
			fprintf(stderr, "zsx: input was truncated while reading\n");
			ok = 0;
		} else {
			zsx_len(chunk) = wanted;
			zsx_encode(chunk);
			uint32_t encoded = (uint32_t)zsx_len(chunk);
			byte length[4];
			put_u32(length, encoded);
			ok = write_all(output, length, sizeof(length)) &&
				write_all(output, chunk, encoded);
			if (!ok) fprintf(stderr, "zsx: write failed for '%s'\n", output_path);
		}
		zsx_del(chunk);
	}
	if (ok && fgetc(input) != EOF) {
		fprintf(stderr, "zsx: input changed while reading\n");
		ok = 0;
	} else if (ok && ferror(input)) {
		fprintf(stderr, "zsx: read failed for '%s'\n", input_path);
		ok = 0;
	}
	fclose(input);
	if (fclose(output) != 0) ok = 0;
	if (ok && !replace_output(temporary, output_path)) {
		fprintf(stderr, "zsx: cannot replace '%s': %s\n", output_path, strerror(errno));
		ok = 0;
	}
	if (!ok) remove(temporary);
	free(temporary);
	if (ok) printf("Compressed %u bytes to %s\n", original, output_path);
	return ok;
}

static int decompress_file(const char *input_path, const char *output_path) {
	ZSX_STAT_STRUCT info;
	if (zsx_stat(input_path, &info) != 0 || !zsx_isreg(info.st_mode)) {
		fprintf(stderr, "zsx: input is not a readable regular file: '%s'\n", input_path);
		return 0;
	}
	if (paths_alias(input_path, output_path)) {
		fprintf(stderr, "zsx: input and output refer to the same file\n");
		return 0;
	}
	FILE *input = fopen(input_path, "rb");
	if (!input) {
		fprintf(stderr, "zsx: cannot open '%s': %s\n", input_path, strerror(errno));
		return 0;
	}
	byte header[16];
	if (!read_all(input, header, sizeof(header))) {
		fprintf(stderr, "zsx: truncated archive header\n");
		fclose(input);
		return 0;
	}
	uint32_t original = get_u32(header + 4);
	uint32_t chunks = get_u32(header + 8);
	uint32_t flags = get_u32(header + 12);
	uint32_t expected_chunks = original == 0 ? 0 :
		(uint32_t)(((uint64_t)original + zsx_chunk - 1) / zsx_chunk);
	if (memcmp(header, "ZSX1", 4) != 0 || flags != 0 || chunks != expected_chunks) {
		fprintf(stderr, "zsx: invalid ZSX1 header, flags, or chunk count\n");
		fclose(input);
		return 0;
	}
	char *temporary = 0;
	FILE *output = create_temp_output(output_path, &temporary);
	if (!output) {
		fprintf(stderr, "zsx: cannot create temporary output for '%s': %s\n", output_path, strerror(errno));
		fclose(input);
		return 0;
	}

	int ok = 1;
	uint64_t produced = 0;
	for (uint32_t i = 0; ok && i < chunks; i++) {
		byte length_bytes[4];
		if (!read_all(input, length_bytes, sizeof(length_bytes))) {
			fprintf(stderr, "zsx: truncated chunk length\n");
			ok = 0;
			break;
		}
		uint32_t length = get_u32(length_bytes);
		if (length < header_sizes || length > zsx_chunk + header_sizes) {
			fprintf(stderr, "zsx: invalid chunk length\n");
			ok = 0;
			break;
		}
		byte *chunk = zsx_new(byte, (size_t)zsx_chunk + header_sizes + 1);
		if (!read_all(input, chunk, length)) {
			fprintf(stderr, "zsx: truncated chunk payload\n");
			ok = 0;
		} else {
			uint32_t declared = get_u32(chunk);
			uint32_t expected = original - (uint32_t)produced;
			if (expected > zsx_chunk) expected = zsx_chunk;
			if (declared != expected || produced + declared > original) {
				fprintf(stderr, "zsx: chunk output length does not match archive\n");
				ok = 0;
			} else {
				zsx_len(chunk) = length;
				if (!zsx_decode(chunk)) {
					fprintf(stderr, "zsx: chunk decode failed\n");
					ok = 0;
				} else if ((uint32_t)zsx_len(chunk) != declared ||
					!write_all(output, chunk, declared)) {
					fprintf(stderr, "zsx: output write or length failure\n");
					ok = 0;
				} else {
					produced += declared;
				}
			}
		}
		zsx_del(chunk);
	}
	if (ok && produced != original) {
		fprintf(stderr, "zsx: decoded output length mismatch\n");
		ok = 0;
	}
	if (ok) {
		int extra = fgetc(input);
		if (extra != EOF || ferror(input)) {
			fprintf(stderr, "zsx: trailing archive data or read failure\n");
			ok = 0;
		}
	}
	fclose(input);
	if (fclose(output) != 0) ok = 0;
	if (ok && !replace_output(temporary, output_path)) {
		fprintf(stderr, "zsx: cannot replace '%s': %s\n", output_path, strerror(errno));
		ok = 0;
	}
	if (!ok) remove(temporary);
	free(temporary);
	if (ok) printf("Decompressed %u bytes to %s\n", original, output_path);
	return ok;
}

static char *default_compressed_name(const char *input) {
	size_t length = strlen(input);
	char *name = (char *)malloc(length + 5);
	if (!name) return 0;
	memcpy(name, input, length);
	memcpy(name + length, ".zsx", 5);
	return name;
}

static char *default_decompressed_name(const char *input) {
	size_t length = strlen(input);
	int has_suffix = length >= 4 && strcmp(input + length - 4, ".zsx") == 0;
	size_t output_length = has_suffix ? length - 4 : length + 4;
	char *name = (char *)malloc(output_length + 1);
	if (!name) return 0;
	if (has_suffix) {
		memcpy(name, input, output_length);
		name[output_length] = 0;
	} else {
		memcpy(name, input, length);
		memcpy(name + length, ".out", 5);
	}
	return name;
}

static void usage(FILE *out, const char *program) {
	fprintf(out,
		"Usage:\n"
		"  %s INPUT [OUTPUT]       Compress to a portable ZSX1 archive\n"
		"  %s -d INPUT [OUTPUT]    Decompress a ZSX1 archive\n"
		"  %s -h | --help          Show this help\n"
		"\nDefault output is INPUT.zsx when compressing; decompression strips\n"
		".zsx or appends .out when that suffix is absent.\n",
		program, program, program);
}

int main(int argc, char **argv) {
	int decode = 0;
	int at = 1;
	if (argc == 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		usage(stdout, argv[0]);
		return 0;
	}
	if (at < argc && !strcmp(argv[at], "-d")) {
		decode = 1;
		at++;
	}
	if (argc - at < 1 || argc - at > 2) {
		usage(stderr, argv[0]);
		return 2;
	}
	const char *input = argv[at];
	char *allocated = 0;
	const char *output = argc - at == 2 ? argv[at + 1] :
		(allocated = decode ? default_decompressed_name(input) : default_compressed_name(input));
	if (!output) {
		fprintf(stderr, "zsx: out of memory\n");
		return 1;
	}
	if (!strcmp(input, output)) {
		fprintf(stderr, "zsx: input and output paths must differ\n");
		free(allocated);
		return 1;
	}
	int ok = decode ? decompress_file(input, output) : compress_file(input, output);
	free(allocated);
	return ok ? 0 : 1;
}

// .::/ zsx_decode /::.
// Decompress a byte buffer using zsx_predict_read.
// .......................
// byte * data 			: the buffer to decompress, header first
// .......................
// returns the decompressed byte* buffer
byte *zsx_decode(byte *data) {

	//read sizes : original length, stream length, literal count, crc
	int len = zsx_len(data);
	unsigned int length = (unsigned)data[0] | ((unsigned)data[1] << 8) |
		((unsigned)data[2] << 16) | ((unsigned)data[3] << 24);
	unsigned int first_len = (unsigned)data[4] | ((unsigned)data[5] << 8) |
		((unsigned)data[6] << 16) | ((unsigned)data[7] << 24);
	unsigned int bytes_len = (unsigned)data[8] | ((unsigned)data[9] << 8) |
		((unsigned)data[10] << 16) | ((unsigned)data[11] << 24);
	unsigned int crc = (unsigned)data[12] | ((unsigned)data[13] << 8) |
		((unsigned)data[14] << 16) | ((unsigned)data[15] << 24);
	if (len < (int)header_sizes || length > zsx_chunk || crc != 0 ||
		first_len != (unsigned)(len - header_sizes))
		return 0;

	//a chunk that was kept as it was : only the header has to go away
	if((int)bytes_len == -1)
	{
		if (first_len != length) return 0;
		memmove(data, data + header_sizes, length);
		zsx_len(data) = length;
		data[length] = 0;
		return data;
	}

	//allocate buffers
	if (length == 0 || bytes_len > length || first_len < 5) return 0;
	byte * result = data;
	byte * control_bits = zsx_new(byte, zsx_buffer);
	memcpy(control_bits, data + header_sizes, len - header_sizes);

	if(!zsx_predict_read(control_bits, result, length, first_len, bytes_len)) {
		zsx_del(control_bits);
		return 0;
	}

	//TODO :  CRC Check
	zsx_del(control_bits);

	return result;
}
