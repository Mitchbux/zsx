// // compile with gcc for windows
// C:\>   bin\gcc zsx.c -o zsx.exe
#define _WIN32_WINNT 0x0602
#define NTDDI_VERSION 0x06020000
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

// .:: / auxilary memory routines /::.
// ...................................

typedef uint8_t byte;
typedef uint16_t ushort;

void *zsx_alloc(size_t item_size, size_t n_item) {
	size_t *x = (size_t *)calloc(1, sizeof(size_t) * 2 + n_item * item_size);
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
	return d->start < d->ends ? (byte)d->data[d->start++] : 0;
}

zsx_decoder_t *zsx_decoder(byte *in, int ends) {
	zsx_decoder_t *d = zsx_new(zsx_decoder_t, 1);
	d->range = 0xFFFFFFFF;
	d->code = 0;
	d->data = in;
	//the first byte the coder emits is the primed cache, it carries nothing
	d->start = 1;
	d->ends = ends;
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
#define zsx_buffer 0x4000000
//chunk size : 12Mb
#define zsx_chunk 0xC00000
#define zsx_word 0x10000
#define zsx_byte 0x100
//the header is : original length, payload length, literal count. The payload is
//the coded stream, nothing in front of it
#define header_sizes (sizeof(int)*3)

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
	int z, s, x;
	int len = zsx_len(data);
	int *value = zsx_new(int, zsx_word);
	int *count = zsx_new(int, zsx_word);
	int *index = zsx_new(int, zsx_word);
	ushort *next_word = (ushort *) data;
	ushort *done = next_word + (len>>1);
	byte *ends = data + len;
	int literals = 0;

	zsx_clear(count);
	while (next_word < done)
		count[*next_word++]++;

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

	z=0;
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
		unsigned int h3 = zsx_most_ctx(z,s,x);
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

		z=s;
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



// .::/ zsx_quick_write /::.
// Write sorted values based on a pivot. The algorithm only write the number of values below the pivot and then 
// iterates through two new pivots in order to find the original values when read
// .......................
// zsx_writer_t *w 	 : the bitwise writer to write to
// ushort *list 	 : the sorted list of values
// int len 			 : the number of elements in the list
// int pivot		 : the pivot, must have pivot*2 greater than any value
// int stack		 : the next interval between the pivot and both the next pivots

void zsx_quick_write(zsx_coder_t *w, int *list, int len, int pivot, int stack) {
	if(len<1) return;

	int s=0; while(list[s]<pivot&&s<len)s++;
	int bits = 1;while((1<<bits)<len+1)bits++;
	zsx_code_direct(w, s, bits);

	if(stack <1) return;

	zsx_quick_write(w, list, s, pivot-stack, stack>>1);
	zsx_quick_write(w, list+s, len-s, pivot+stack, stack>>1);
}

// .::/ zsx_quick_read /::.
// Read sorted values based on a pivot. 
// .......................
// zsx_reader_t *r 	: the bitwise writer to write to
// int *index	    : the initial index of elements
// int *value		: the final value list
// int len 			: the number of elements in the index list
// int pivot : should be the same when used with zsx_quick_write
// int stack : should be the same when used with zsx_quick_write

void zsx_quick_read(zsx_decoder_t *r, int *index, int *value, int len, int pivot, int stack) {
	if(len<1) return;

	int bits = 1;while((1<<bits)<len+1)bits++;
	int s = zsx_decode_direct(r, bits);

	if(stack<1){

		for(int z=0;z<len;z++)
			if(z<s) value[index[z]]=pivot-1;
			else    value[index[z]]=pivot;
		return;
	}

	zsx_quick_read(r, index, value, s, pivot-stack, stack>>1);
	zsx_quick_read(r, index+s, value, len-s, pivot+stack, stack>>1);
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
	else memcpy(data + header_sizes, buffer, first_len);

	//set size header
	((int*)(data))[0] = length;
	((int*)(data))[1] = first_len;
	((int*)(data))[2] = bytes_len;

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
			if(decoded+2 > len) {fail = 1; break;}
			result[decoded++] = index[z];
			result[decoded++] = index[z]>>8;
		}else
			result[decoded++] = z;

		//we save the two last tokens for prediction
		r=s;
		s=x;
		x=z;
	}

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
	if(fail || decoded != len || missed != bytes_len) return 0;

	zsx_len(result) = len;
	result[len] = 0;

	return result;
}

// .::/ zsx_decode /::.
// Decompress a byte buffer using zsx_predict_read.
// .......................
// byte * data 			: the buffer to decompress, header first
// .......................
// returns the decompressed byte* buffer
byte *zsx_decode(byte *data) {

	//read sizes : original length, payload length, literal count
	int len = zsx_len(data);
	unsigned int length 	= ((int*)data)[0];
	unsigned int first_len	= ((int*)data)[1];
	int bytes_len			= ((int*)data)[2];

	//nothing zsx_encode ever wrote is longer than one buffer, and none of it
	//reaches past what the chunk actually holds. A header that says otherwise
	//is a damaged one, and it is caught before anything is moved on its word
	if(len < (int)header_sizes || length >= zsx_buffer || first_len >= zsx_buffer
		|| len < (int)(header_sizes + first_len))
	{
		printf("\nDecoding failed!\n");
		return data;
	}

	//a chunk that was kept as it was : only the header has to go away
	if(bytes_len == -1)
	{
		memmove(data, data + header_sizes, length);
		zsx_len(data) = length;
		data[length] = 0;
		return data;
	}

	//allocate buffers
	byte * result = data;
	//what comes out is longer than what it came from, so the decoded bytes
	//would run over the payload still being read : the payload steps aside
	byte * packed = zsx_new(byte, first_len + 0x10);
	memcpy(packed, data + header_sizes, first_len);

	//and the contexts read it back the way they wrote it
	if(!zsx_predict_read(packed, result, length, first_len, bytes_len))
		printf("\nDecoding failed!\n");

	//TODO :  CRC Check
	zsx_del(packed);

	return result;
}

int zsx_bytes_read;

void CALLBACK FIO(DWORD E1, DWORD N2, LPOVERLAPPED ol) {
  zsx_bytes_read = N2;
}

#define max_thread 0x10

int decode = 0;
int high = 0;
volatile LONG zsx_mismatch = 0;

DWORD WINAPI ThreadFunc(LPVOID data)
{
	if(decode)
	{
		byte * buffer = (byte*)data;
		size_t initial = zsx_len(buffer);

		//keep a copy of the chunk so the round trip can be checked
		byte * source = zsx_new(byte, initial+1);
		memcpy(source, buffer, initial);

		buffer = zsx_encode(buffer);
		buffer = zsx_decode(buffer);

		if(zsx_len(buffer)!=initial || memcmp(source, buffer, initial))
			InterlockedIncrement(&zsx_mismatch);

		zsx_del(source);
	}else
	{
		byte * buffer = (byte*)data;
		size_t initial = zsx_len(data);

		buffer = zsx_encode(buffer);

		size_t len_zsx = zsx_len(buffer);

		double ratio = (double) len_zsx / (double) initial;
		printf("\r%.2f%% ", 100.0 - ratio*100.0);
		if(high)
		{
			size_t prev_len = initial;
			while(prev_len>len_zsx)
			//for(int z=0;z<8;z++)
			{
				prev_len = len_zsx;
				buffer = zsx_encode(buffer);
				//buffer = zsx_encode(buffer);


				len_zsx = zsx_len(buffer);
				ratio = (double) len_zsx / (double) initial;
				printf("\r%.2f%% ", 100.0 - ratio*100.0);
			}
		}

	}
	return 0;
}


int test(char *filename) {

	//win32 file handling

	int start = GetTickCount();

	OVERLAPPED ol;
	memset(&ol, 0, sizeof(ol));

	HANDLE hFile = CreateFile(filename,GENERIC_READ,
		FILE_SHARE_READ,NULL,OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,NULL);

	if (hFile == INVALID_HANDLE_VALUE) {
		printf("Can't read file!\n");
		return 0;
	};

	int encoded = 0;
    DWORD file_high;
    int size = GetFileSize(hFile, &file_high);
	uint64_t file_size = size;
	char *outfilename = zsx_new(char, 0x100);
	strcpy(outfilename, filename);
	strcat(outfilename, ".zsx");

	FILE *f = fopen(outfilename, "wb");

	if (f == NULL) {
		printf("Can't create file!\n");
		zsx_del(outfilename);
		CloseHandle(hFile);
		return 0;
	};

	printf("\nMulti-thread Encoding...\n");

	//zsx library has a few set of macros.
	//this one takes care of everything about initialization
	HANDLE *threads = zsx_new(HANDLE, max_thread);
	DWORD *ids = zsx_new(DWORD, max_thread);
	byte ** results = zsx_new(byte*, max_thread);
	int next_thread = 0;
	size_t bytesWritten = 0;

		while (encoded < file_size)
		{
			results[next_thread]=zsx_new(byte, zsx_buffer);
				zsx_bytes_read = 0;
				ReadFileEx(hFile, results[next_thread], zsx_chunk, &ol, FIO);
				SleepEx(INFINITE, TRUE);
				encoded += zsx_bytes_read;
				ol.Offset += zsx_bytes_read;
			//a short read means the file ended early : stop instead of looping
			if(zsx_bytes_read == 0)
			{
				zsx_del(results[next_thread]);
				break;
			}
			printf("\r       Bytes read : %d", zsx_bytes_read);
			zsx_len(results[next_thread]) = zsx_bytes_read;
			threads[next_thread] = CreateThread(NULL, 0, ThreadFunc, (LPVOID)results[next_thread], 0, &ids[next_thread]);
			next_thread++;
			if(next_thread==max_thread)
			{

				WaitForMultipleObjects(next_thread, threads, TRUE, INFINITE);

				for(int s=0;s<next_thread;s++)
				{
					//file writing
					size_t len_zsx = zsx_len(results[s]);
					bytesWritten += len_zsx;
					if(decode==0)
					fwrite(&len_zsx, sizeof(size_t), 1, f);
					fwrite(results[s], len_zsx, 1, f);
					zsx_del(results[s]);
					CloseHandle(threads[s]);
				}

				next_thread=0;
				printf("               Total : %.2f%%", ((double)encoded / (double)file_size)*100.0);
			}
		}

		//waiting on an empty set is an error, the last batch can be empty
		if(next_thread)
			WaitForMultipleObjects(next_thread, threads, TRUE, INFINITE);


		for(int s=0;s<next_thread;s++)
		{
			//file writing
			size_t len_zsx = zsx_len(results[s]);
			bytesWritten += len_zsx;
			if(decode==0)
			fwrite(&len_zsx, sizeof(size_t), 1, f);
			fwrite(results[s], len_zsx, 1, f);
			zsx_del(results[s]);
			CloseHandle(threads[s]);
		}
		if(next_thread)
		printf("               Total : %.2f%%", ((double)encoded / (double)file_size)*100.0);

		double ratio = (double)bytesWritten / (double)encoded;
		printf("\rRatio : %.8f%%. ", ratio*100.0);

	fclose(f);
	CloseHandle(hFile);
	if(decode==0) printf("\nCompressed to %s.\n", outfilename);
	else printf("\nDecompressed to %s. Round trip : %s\n", outfilename,
		zsx_mismatch ? "MISMATCH" : "identical");
	int elapsed = GetTickCount() - start;
	printf("Took %d seconds.", elapsed/1000);

	//the per run buffers go back to the heap
	zsx_del(threads);
	zsx_del(ids);
	zsx_del(results);
	zsx_del(outfilename);
	return 1;
}

// .:: / ZSX2 archive /::.
// One file holds many : whole folders, and whatever a wildcard matched, each
// one cut into chunks the coder already knows how to handle.
// .......................
// "ZSX2"                  the magic
// per entry :
//   int    name_len       zero ends the archive
//   char   name[name_len] relative, forward slashed, no drive and no ".."
//   byte   kind           0 a file, 1 a folder
//   int64  size           what the file held, for the listing
//   per chunk :
//     int  len            zero ends the chunks of this entry
//     byte data[len]      one zsx_encode chunk, its own header first
// .................................

#define zsx2_magic "ZSX2"

char *zsx_str(const char *s) {
	size_t n = strlen(s);
	char *x = zsx_new(char, n+1);
	memcpy(x, s, n+1);
	return x;
}

//sticks a name under a path, unless the path already ends on a separator
char *zsx_join(const char *a, const char *b) {
	size_t n = strlen(a), m = strlen(b);
	if(!n) return zsx_str(b);
	int sep = !(a[n-1]=='/' || a[n-1]=='\\' || a[n-1]==':');
	char *x = zsx_new(char, n+m+2);
	memcpy(x, a, n);
	if(sep) x[n++] = '/';
	memcpy(x+n, b, m+1);
	return x;
}

//a name inside the archive is relative and forward slashed : the drive, the
//leading separators and every step back up the tree are dropped here, so that
//nothing unpacked can ever land outside the folder it was asked to land in
char *zsx_clean(const char *path) {
	char *out = zsx_new(char, strlen(path)+1);
	const char *s = path;
	int n = 0;
	if(s[0] && s[1]==':') s+=2;
	while(*s=='/' || *s=='\\') s++;
	while(*s)
	{
		const char *e = s;
		while(*e && *e!='/' && *e!='\\') e++;
		int len = e-s;
		if(!(len==1 && s[0]=='.') && !(len==2 && s[0]=='.' && s[1]=='.'))
		{
			if(n) out[n++]='/';
			memcpy(out+n, s, len);
			n += len;
		}
		s = e;
		while(*s=='/' || *s=='\\') s++;
	}
	out[n]=0;
	return out;
}

//and the same read the other way round : a name that came out of an archive
//somebody else made is only trusted if it could have come out of zsx_clean
int zsx_safe(const char *name) {
	if(!*name || name[0]=='/' || name[0]=='\\' || name[1]==':') return 0;
	for(const char *s=name;*s;s++)
		if(s[0]=='.' && s[1]=='.' && (!s[2] || s[2]=='/' || s[2]=='\\')
			&& (s==name || s[-1]=='/' || s[-1]=='\\')) return 0;
	return 1;
}

//every folder on the way to a file is made before the file is
void zsx_mkpath(const char *path) {
	char *x = zsx_str(path);
	for(char *s=x;*s;s++)
		if(*s=='/' || *s=='\\')
		{
			char t = *s;
			*s = 0;
			if(s!=x && s[-1]!=':') CreateDirectory(x, NULL);
			*s = t;
		}
	zsx_del(x);
}

typedef struct {
	char *path;		//where it is on disk
	char *name;		//what it is called inside the archive
	int folder;
} zsx_entry_t;

typedef struct {
	zsx_entry_t *item;
	int count;
	int room;
} zsx_list_t;

void zsx_list_add(zsx_list_t *l, const char *path, const char *name, int folder) {
	if(l->count == l->room)
	{
		int room = l->room ? l->room*2 : 0x40;
		zsx_entry_t *item = zsx_new(zsx_entry_t, room);
		for(int s=0;s<l->count;s++)
			item[s] = l->item[s];
		if(l->item) zsx_del(l->item);
		l->item = item;
		l->room = room;
	}
	l->item[l->count].path = zsx_str(path);
	l->item[l->count].name = zsx_str(name);
	l->item[l->count].folder = folder;
	l->count++;
}

void zsx_list_del(zsx_list_t *l) {
	for(int s=0;s<l->count;s++)
		zsx_del(l->item[s].path), zsx_del(l->item[s].name);
	if(l->item) zsx_del(l->item);
	l->item = 0;
	l->count = l->room = 0;
}

//walks a folder down to its leaves. A junction is stepped over rather than
//followed : it can point back up the tree and the walk would never end
void zsx_scan(zsx_list_t *l, const char *path, const char *name) {
	char *pattern = zsx_join(path, "*");
	WIN32_FIND_DATA fd;
	HANDLE h = FindFirstFile(pattern, &fd);
	int found = 0;
	if(h != INVALID_HANDLE_VALUE)
	{
		do {
			if(!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
			if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
			char *child = zsx_join(path, fd.cFileName);
			char *label = zsx_join(name, fd.cFileName);
			if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				zsx_scan(l, child, label);
			else
				zsx_list_add(l, child, label, 0);
			found = 1;
			zsx_del(child);
			zsx_del(label);
		} while(FindNextFile(h, &fd));
		FindClose(h);
	}
	//a folder with nothing in it has no file to carry it, so it is named itself
	if(!found) zsx_list_add(l, path, name, 1);
	zsx_del(pattern);
}

//one thing asked for on the command line : a file, a folder taken whole, or a
//wildcard. Win32 matches the wildcard, and a folder it matched is taken whole
//too, so -a out.zsx2 src\* picks up everything under src
void zsx_add(zsx_list_t *l, const char *arg) {
	if(strchr(arg,'*') || strchr(arg,'?'))
	{
		//the pattern is matched where it lives, which is the path in front of
		//its last separator
		size_t n = strlen(arg);
		while(n && arg[n-1]!='/' && arg[n-1]!='\\' && arg[n-1]!=':') n--;
		char *base = zsx_new(char, n+1);
		memcpy(base, arg, n);
		base[n] = 0;
		char *label = zsx_clean(base);
		WIN32_FIND_DATA fd;
		HANDLE h = FindFirstFile(arg, &fd);
		int found = 0;
		if(h != INVALID_HANDLE_VALUE)
		{
			do {
				if(!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
				if(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
				char *child = zsx_join(base, fd.cFileName);
				char *name = zsx_join(label, fd.cFileName);
				if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					zsx_scan(l, child, name);
				else
					zsx_list_add(l, child, name, 0);
				found = 1;
				zsx_del(child);
				zsx_del(name);
			} while(FindNextFile(h, &fd));
			FindClose(h);
		}
		if(!found) printf("Nothing matches %s\n", arg);
		zsx_del(base);
		zsx_del(label);
		return;
	}

	DWORD at = GetFileAttributes(arg);
	if(at == INVALID_FILE_ATTRIBUTES)
	{
		printf("Can't read %s\n", arg);
		return;
	}
	char *name = zsx_clean(arg);
	if(!*name)
		printf("Can't name %s inside an archive\n", arg);
	else if(at & FILE_ATTRIBUTE_DIRECTORY)
		zsx_scan(l, arg, name);
	else
		zsx_list_add(l, arg, name, 0);
	zsx_del(name);
}

DWORD WINAPI zsx_pack_thread(LPVOID data) {
	zsx_encode((byte*)data);
	return 0;
}

DWORD WINAPI zsx_unpack_thread(LPVOID data) {
	zsx_decode((byte*)data);
	return 0;
}

//waits on a batch of chunks and writes what they turned into. They are written
//in the order they were read whatever order they finished in
uint64_t zsx_batch(FILE *f, HANDLE *threads, byte **chunk, int live, int packing) {
	uint64_t written = 0;
	for(int s=0;s<live;s++)
	{
		if(threads[s])
			WaitForSingleObject(threads[s], INFINITE), CloseHandle(threads[s]);
		size_t len = zsx_len(chunk[s]);
		if(packing)
		{
			int coded = len;
			if(f) fwrite(&coded, sizeof(int), 1, f);
		}
		if(f) fwrite(chunk[s], 1, len, f);
		written += len;
		zsx_del(chunk[s]);
	}
	return written;
}

// .::/ zsx_pack /::.
// Builds a ZSX2 archive out of files, folders and wildcards.
// .......................
// returns 1 when the archive was written
int zsx_pack(const char *outname, char **args, int count) {
	zsx_list_t list;
	memset(&list, 0, sizeof(list));
	for(int s=0;s<count;s++)
		zsx_add(&list, args[s]);

	if(!list.count)
	{
		printf("Nothing to pack.\n");
		zsx_list_del(&list);
		return 0;
	}

	FILE *f = fopen(outname, "wb");
	if(!f)
	{
		printf("Can't create %s\n", outname);
		zsx_list_del(&list);
		return 0;
	}

	int start = GetTickCount();
	fwrite(zsx2_magic, 4, 1, f);
	printf("\nPacking %d entries into %s...\n", list.count, outname);

	HANDLE *threads = zsx_new(HANDLE, max_thread);
	byte **chunk = zsx_new(byte*, max_thread);
	uint64_t total_in = 0, total_out = 0;
	int stored = 0;

	for(int s=0;s<list.count;s++)
	{
		HANDLE hFile = INVALID_HANDLE_VALUE;
		byte kind = list.item[s].folder ? 1 : 0;
		uint64_t size = 0;

		if(!kind)
		{
			hFile = CreateFile(list.item[s].path, GENERIC_READ, FILE_SHARE_READ,
				NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			if(hFile == INVALID_HANDLE_VALUE)
			{
				printf("Can't read %s, skipped.\n", list.item[s].path);
				continue;
			}
			LARGE_INTEGER li;
			if(GetFileSizeEx(hFile, &li)) size = li.QuadPart;
		}

		int name_len = strlen(list.item[s].name);
		fwrite(&name_len, sizeof(int), 1, f);
		fwrite(list.item[s].name, name_len, 1, f);
		fwrite(&kind, 1, 1, f);
		fwrite(&size, sizeof(uint64_t), 1, f);
		stored++;

		int live = 0;
		while(!kind)
		{
			byte *buffer = zsx_new(byte, zsx_buffer);
			DWORD got = 0;
			//a read that brought nothing back is the end of the file
			if(!ReadFile(hFile, buffer, zsx_chunk, &got, NULL) || !got)
			{
				zsx_del(buffer);
				break;
			}
			zsx_len(buffer) = got;
			total_in += got;
			chunk[live] = buffer;
			threads[live] = CreateThread(NULL, 0, zsx_pack_thread, (LPVOID)buffer, 0, NULL);
			//no thread to be had is not a reason to lose the chunk
			if(!threads[live]) zsx_encode(buffer);
			live++;
			if(live == max_thread)
				total_out += zsx_batch(f, threads, chunk, live, 1), live = 0;
		}
		if(live) total_out += zsx_batch(f, threads, chunk, live, 1);

		//the chunks of this entry are over
		int ends = 0;
		fwrite(&ends, sizeof(int), 1, f);
		if(hFile != INVALID_HANDLE_VALUE) CloseHandle(hFile);

		printf("\r%-52.52s %6.2f%% ", list.item[s].name,
			total_in ? 100.0 - ((double)total_out/(double)total_in)*100.0 : 0.0);
	}

	//and a name of no length at all is the end of the archive
	int ends = 0;
	fwrite(&ends, sizeof(int), 1, f);
	fclose(f);

	printf("\rPacked %d entries, %llu bytes into %llu. Ratio : %.4f%%.\n",
		stored, (unsigned long long)total_in, (unsigned long long)total_out,
		total_in ? ((double)total_out/(double)total_in)*100.0 : 0.0);
	printf("Took %d seconds.\n", (int)(GetTickCount()-start)/1000);

	zsx_del(threads);
	zsx_del(chunk);
	zsx_list_del(&list);
	return 1;
}

// .::/ zsx_unpack /::.
// Reads a ZSX2 archive back out. With naming on, nothing is written : the
// entries are only named and measured.
// .......................
// returns 1 when the whole archive was read
int zsx_unpack(const char *inname, const char *root, int naming) {
	FILE *in = fopen(inname, "rb");
	if(!in)
	{
		printf("Can't read %s\n", inname);
		return 0;
	}

	char magic[4];
	if(fread(magic, 1, 4, in)!=4 || memcmp(magic, zsx2_magic, 4))
	{
		printf("%s is not a ZSX2 archive.\n", inname);
		fclose(in);
		return 0;
	}

	int start = GetTickCount();
	HANDLE *threads = zsx_new(HANDLE, max_thread);
	byte **chunk = zsx_new(byte*, max_thread);
	char *name = zsx_new(char, 0x1000);
	uint64_t total_in = 0, total_out = 0;
	int count = 0, fail = 0;

	if(!naming) printf("\nUnpacking %s...\n", inname);

	for(;;)
	{
		int name_len = 0;
		if(fread(&name_len, sizeof(int), 1, in)!=1) {fail=1; break;}
		//a name of no length at all is the end of the archive
		if(!name_len) break;
		if(name_len < 0 || name_len >= 0x1000) {fail=1; break;}
		if(fread(name, 1, name_len, in)!=(size_t)name_len) {fail=1; break;}
		name[name_len] = 0;

		byte kind = 0;
		uint64_t size = 0;
		if(fread(&kind, 1, 1, in)!=1) {fail=1; break;}
		if(fread(&size, sizeof(uint64_t), 1, in)!=1) {fail=1; break;}

		//a name that could not have come out of zsx_clean is not followed
		int keep = zsx_safe(name);
		if(!keep) printf("Refusing %s\n", name);

		char *full = zsx_join(root, name);
		FILE *out = 0;
		if(!naming && keep)
		{
			zsx_mkpath(full);
			if(kind) CreateDirectory(full, NULL);
			else if(!(out = fopen(full, "wb")))
				printf("Can't create %s\n", full);
		}
		if(naming) printf("%12llu  %s%s\n", (unsigned long long)size, name, kind?"/":"");
		count++;

		int live = 0;
		for(;;)
		{
			int len = 0;
			if(fread(&len, sizeof(int), 1, in)!=1) {fail=1; break;}
			//and a chunk of no length at all is the end of this entry
			if(!len) break;
			if(len < 0 || len > zsx_buffer) {fail=1; break;}
			byte *buffer = zsx_new(byte, zsx_buffer);
			if(fread(buffer, 1, len, in)!=(size_t)len) {zsx_del(buffer); fail=1; break;}
			zsx_len(buffer) = len;
			total_in += len;
			if(naming || !keep)
			{
				zsx_del(buffer);
				continue;
			}
			chunk[live] = buffer;
			threads[live] = CreateThread(NULL, 0, zsx_unpack_thread, (LPVOID)buffer, 0, NULL);
			if(!threads[live]) zsx_decode(buffer);
			live++;
			if(live == max_thread)
				total_out += zsx_batch(out, threads, chunk, live, 0), live = 0;
		}
		if(live) total_out += zsx_batch(out, threads, chunk, live, 0);

		if(out) fclose(out);
		if(!naming && keep && !kind)
			printf("\r%-52.52s ", name);
		zsx_del(full);
		if(fail) break;
	}

	fclose(in);
	zsx_del(threads);
	zsx_del(chunk);
	zsx_del(name);

	if(fail)
	{
		printf("\n%s ends short or is damaged.\n", inname);
		return 0;
	}
	if(naming)
		printf("%d entries, %llu bytes stored.\n", count, (unsigned long long)total_in);
	else
	{
		printf("\rUnpacked %d entries, %llu bytes into %llu.\n", count,
			(unsigned long long)total_in, (unsigned long long)total_out);
		printf("Took %d seconds.\n", (int)(GetTickCount()-start)/1000);
	}
	return 1;
}

void zsx_usage(const char *self) {
	printf("Usage : %s file                 compress one file to file.zsx\n", self);
	printf("        %s -d file              round trip a file through the coder\n", self);
	printf("        %s -h file              compress it twice over\n", self);
	printf("        %s -a out.zsx2 path...  pack files, folders and wildcards\n", self);
	printf("        %s -x out.zsx2 [into]   unpack an archive, here by default\n", self);
	printf("        %s -l out.zsx2          name what an archive holds\n", self);
}

int main(int argc, char **argv) {

	if(argc<2)
	{
		zsx_usage(argv[0]);
	}else if (argv[1][0]=='-' && argv[1][1]=='a')
	{
		if(argc<4) zsx_usage(argv[0]);
		else zsx_pack(argv[2], argv+3, argc-3);
	}else if (argv[1][0]=='-' && argv[1][1]=='x')
	{
		if(argc<3) zsx_usage(argv[0]);
		else zsx_unpack(argv[2], argc>3 ? argv[3] : ".", 0);
	}else if (argv[1][0]=='-' && argv[1][1]=='l')
	{
		if(argc<3) zsx_usage(argv[0]);
		else zsx_unpack(argv[2], ".", 1);
	}else if (argv[1][0]=='-' && argv[1][1]=='d')
	{
		if(argc<3) zsx_usage(argv[0]);
		else decode = 1, test(argv[2]);
	}else if (argv[1][0]=='-' && argv[1][1]=='h')
	{
		if(argc<3) zsx_usage(argv[0]);
		else high = 1, test(argv[2]);
	}else if (argc==2)
	{
		test(argv[1]);
	}else
	{
		zsx_usage(argv[0]);
	}
	return 0;

}
