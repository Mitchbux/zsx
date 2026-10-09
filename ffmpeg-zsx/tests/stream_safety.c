/* Link with --wrap=malloc,--wrap=calloc,--wrap=free for deterministic OOM.
 * The tracker is test-only; production libraries have no global allocator. */
#include "../src/zsx_core_api.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t);
void __real_free(void *);
static size_t calls, fail_at, live, peak;
typedef union { max_align_t alignment; size_t size; } Header;
void *__wrap_malloc(size_t n) {
    if (++calls==fail_at || n>SIZE_MAX-sizeof(Header)) return NULL;
    Header *p=__real_malloc(n+sizeof(Header));
    if (!p) return NULL;
    p->size=n; live+=n; if(live>peak) peak=live;
    return p+1;
}
void *__wrap_calloc(size_t n, size_t size) {
    if (size && n>SIZE_MAX/size) return NULL;
    void *p=__wrap_malloc(n*size);
    if(p) memset(p,0,n*size);
    return p;
}
void __wrap_free(void *p) {
    if(p) { Header *h=(Header *)p-1; live-=h->size; __real_free(h); }
}

static void put32(uint8_t *p,uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static uint8_t input[4096];
static void rejected(const uint8_t *p,size_t n) {
    uint8_t *out=(void *)(uintptr_t)1;
    size_t len=99;
    assert(zsx_stream_decode(p,n,&out,&len)<0);
    assert(out==(void *)(uintptr_t)1 && len==99);
}
static void oom_stream(unsigned profile) {
    uint8_t *coded=NULL,*out=NULL;
    size_t n=0,len=0,count;
    calls=0;
    assert(!zsx_stream_encode(input,sizeof(input),profile,&coded,&n));
    count=calls;
    free(coded);
    for(size_t i=1;i<=count;i++) {
        calls=0; fail_at=i; coded=(void *)(uintptr_t)1; n=99;
        assert(zsx_stream_encode(input,sizeof(input),profile,&coded,&n)==ZSX_NOMEM);
        assert(coded==(void *)(uintptr_t)1 && n==99 && !live);
    }
    fail_at=0;
    assert(!zsx_stream_encode(input,sizeof(input),profile,&coded,&n));
    size_t base=live;
    calls=0;
    assert(!zsx_stream_decode(coded,n,&out,&len));
    count=calls; free(out);
    for(size_t i=1;i<=count;i++) {
        calls=0; fail_at=i; out=(void *)(uintptr_t)1; len=99;
        assert(zsx_stream_decode(coded,n,&out,&len)==ZSX_NOMEM);
        assert(out==(void *)(uintptr_t)1 && len==99 && live==base);
    }
    fail_at=0; free(coded); assert(!live);
}
static void oom_legacy(int decode) {
    uint8_t *encoded=NULL;
    size_t n=0;
    assert(!zsx_ffmpeg_encode(input,sizeof(input),&encoded,&n));
    size_t base=live;
    const uint8_t *src=decode?encoded:input;
    size_t size=decode?n:sizeof(input);
    int (*fn)(const uint8_t *,size_t,uint8_t **,size_t *)=
        decode?zsx_ffmpeg_decode:zsx_ffmpeg_encode;
    uint8_t *out=NULL; size_t len=0;
    calls=0; assert(!fn(src,size,&out,&len));
    size_t count=calls; zsx_ffmpeg_free(out);
    for(size_t i=1;i<=count;i++) {
        calls=0; fail_at=i; out=(void *)(uintptr_t)1; len=99;
        assert(fn(src,size,&out,&len)==ZSX_NOMEM);
        assert(out==(void *)(uintptr_t)1 && len==99 && live==base);
    }
    fail_at=0; zsx_ffmpeg_free(encoded); assert(!live);
}
int main(void) {
    for(size_t i=0;i<sizeof(input);i++) input[i]=(i%19==0)?(uint8_t)i:(uint8_t)(i%7);
    for(unsigned p=1;p<=4;p++) {
        uint8_t *coded=NULL,*out=NULL;
        size_t n=0,len=0;
        assert(!zsx_stream_encode(input,sizeof(input),p,&coded,&n));
        assert(!zsx_stream_decode(coded,n,&out,&len));
        assert(len==sizeof(input) && !memcmp(out,input,len)); free(out);
        assert(n<sizeof(input));
        for(size_t cut=0;cut<n;cut++) {
            /* Adjust the outer length too: exercise range EOF, not only framing. */
            uint8_t saved[4]; memcpy(saved,coded+12,4);
            if(cut>=24) put32(coded+12,cut-24);
            rejected(coded,cut); memcpy(coded+12,saved,4);
        }
        for(size_t pos=0;pos<24;pos+=4) {
            uint8_t saved[4]; memcpy(saved,coded+pos,4);
            memset(coded+pos,255,4); rejected(coded,n);
            memcpy(coded+pos,saved,4);
        }
        coded[24]=1; rejected(coded,n); coded[24]=0;
        coded[n-1]^=1; rejected(coded,n); coded[n-1]^=1;
        free(coded); assert(!live);
        oom_stream(p);
    }
    oom_legacy(0); oom_legacy(1);
    /* Optional-candidate OOM must keep a valid baseline, without leaking. */
    uint8_t *out=NULL; size_t len=0;
    calls=0; assert(!zsx_ffmpeg_encode(input,sizeof(input),&out,&len));
    size_t baseline_calls=calls; zsx_ffmpeg_free(out);
    calls=0; fail_at=baseline_calls+1;
    assert(!zsx_ffmpeg_encode_best(input,sizeof(input),&out,&len));
    fail_at=0;
    uint8_t *raw=NULL; size_t rawlen=0;
    assert(!zsx_ffmpeg_decode_profile(out,len,&raw,&rawlen));
    assert(rawlen==sizeof(input) && !memcmp(raw,input,rawlen));
    zsx_ffmpeg_free(out); zsx_ffmpeg_free(raw);
    assert(!live);
    assert(peak<128u*1024u*1024u);
    printf("Streaming profiles, truncation, OOM at every allocation: PASS; peak=%zu bytes\n",peak);
    return 0;
}
