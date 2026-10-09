/*
 * Portable one-pass byte/two-candidate predictors ported from the uploaded
 * zsx_stream_candidates model. No archive, Win32, or global model state.
 * A model is at most 39 MiB; all lengths are capped at one 12 MiB chunk.
 */
#include "zsx_stream.h"
#include <stdlib.h>
#include <string.h>

#define CAP (12u * 1024u * 1024u)
#define TOP (1u << 24)
#define HEADER 24u

typedef struct { uint8_t a, b, ca, cb; } Candidate;
typedef struct {
    uint16_t *second, *hits, *hint, *deep, *miss, *tops, *tips, *alts, *altips;
    void *large;
    unsigned bits, candidates, r, s, x;
} Model;
typedef struct {
    uint64_t low;
    uint32_t range, code;
    size_t at, size, pending;
    uint8_t cache, *out;
    const uint8_t *in;
    int error, decode;
} Coder;

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1]<<8 |
           (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void put32(uint8_t *p, uint32_t v) {
    p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24;
}
static uint16_t *probs(size_t n, int init) {
    uint16_t *p=calloc(n,sizeof(*p));
    if (p && init) for (size_t i=0;i<n;i++) p[i]=1024;
    return p;
}
static void model_free(Model *m) {
    free(m->large); free(m->second); free(m->hits); free(m->hint);
    free(m->deep); free(m->miss); free(m->tops); free(m->tips);
    free(m->alts); free(m->altips);
}
static int model_new(Model *m, unsigned profile) {
    memset(m,0,sizeof(*m));
    if (profile<1 || profile>4) return -1;
    m->bits=(profile==2 || profile==4)?20:16;
    m->candidates=profile>=3;
    m->large=calloc((size_t)1<<m->bits,m->candidates?sizeof(Candidate):sizeof(uint16_t));
    m->second=probs(65536,0); m->hits=probs(65536,0);
    m->hint=probs(256,1); m->deep=probs(16777216,0);
    m->miss=probs(65536,1);
    m->tops=probs(m->candidates?4*65536:65536,0);
    m->tips=probs(m->candidates?4*256:256,1);
    m->alts=probs(4*65536,0); m->altips=probs(4*256,1);
    if (!m->large || !m->second || !m->hits || !m->hint ||
        !m->deep || !m->miss || !m->tops || !m->tips || !m->alts || !m->altips) {
        model_free(m); return -2;
    }
    return 0;
}
static void update(uint16_t *p, int bit) {
    if (!bit) *p+=(2048-*p)>>4; else *p-=*p>>4;
}
static void carry(Coder *c) {
    if ((uint32_t)(c->low>>32) || (uint32_t)c->low<0xff000000u) {
        uint8_t t=c->cache;
        do {
            if (c->at==c->size) { c->error=1; return; }
            c->out[c->at++]=t+(uint8_t)(c->low>>32);
            t=255;
        } while (--c->pending);
        c->cache=(uint32_t)c->low>>24;
    }
    c->pending++;
    c->low=(uint32_t)c->low<<8;
}
static uint8_t next(Coder *c) {
    if (c->at==c->size) { c->error=1; return 0; }
    return c->in[c->at++];
}
static int bit(Coder *c, uint16_t *p, int value) {
    uint32_t bound;
    if (c->error) return 0;
    if (!*p || *p>=2048 || (c->decode && c->code>=c->range)) {
        c->error=1; return 0;
    }
    bound=(c->range>>11)* *p;
    if (c->decode) value=c->code>=bound;
    if (!value) c->range=bound;
    else {
        c->range-=bound;
        if (c->decode) c->code-=bound; else c->low+=bound;
    }
    update(p,value);
    while (!c->error && c->range<TOP) {
        c->range<<=8;
        if (c->decode) c->code=(c->code<<8)|next(c); else carry(c);
    }
    return value;
}
static int flag(Coder *c, uint16_t *p, uint16_t *prior, int match) {
    if (!*p) *p=*prior;
    int b=bit(c,p,match);
    update(prior,b);
    return b;
}
static void candidate_update(Candidate *c, int v) {
    if (c->ca+c->cb>=32) { c->ca=(c->ca+1)/2; c->cb=(c->cb+1)/2; }
    if (c->ca && c->a==v) c->ca++;
    else if (c->cb && c->b==v) c->cb++;
    else {
        if (c->ca) c->ca--;
        if (c->cb) c->cb--;
        if (!c->ca) { c->a=v; c->ca=1; }
        else if (!c->cb) { c->b=v; c->cb=1; }
    }
    if (c->cb>c->ca) {
        uint8_t v=c->a,n=c->ca;
        c->a=c->b; c->ca=c->cb; c->b=v; c->cb=n;
    }
}
static int step(Model *m, Coder *rc, int v, uint32_t *misses) {
    unsigned h=((m->r<<16)+(m->s<<8)+m->x)*0x9e3779b1u;
    unsigned h2=m->s*256+m->x;
    int hit=0;
    h>>=32-m->bits;
    Candidate *c=m->candidates?(Candidate *)m->large+h:NULL;
    uint16_t *most=m->candidates?NULL:(uint16_t *)m->large+h;
    if (c && c->ca>=2) {
        unsigned bin=c->ca<4?0:c->ca<8?1:c->ca>=2*c->cb?3:2;
        hit=flag(rc,m->tops+bin*65536+h2,m->tips+bin*256+m->x,v==c->a);
        if (hit && rc->decode) v=c->a;
    } else if (most && *most) {
        hit=flag(rc,m->tops+h2,m->tips+m->x,v==*most-1);
        if (hit && rc->decode) v=*most-1;
    }
    if (!hit && c && c->cb>=2) {
        unsigned bin=c->cb<4?0:c->cb<8?1:c->cb*2>=c->ca?3:2;
        hit=flag(rc,m->alts+bin*65536+h2,m->altips+bin*256+m->x,v==c->b);
        if (hit && rc->decode) v=c->b;
    }
    if (!hit && m->second[h2]) {
        hit=flag(rc,m->hits+h2,m->hint+m->x,v==m->second[h2]-1);
        if (hit && rc->decode) v=m->second[h2]-1;
    }
    if (!hit) {
        uint16_t *deep=m->deep+h2*256, *wide=m->miss+m->x*256;
        unsigned node=1;
        for (int i=7;i>=0;i--) {
            if (!deep[node]) deep[node]=wide[node];
            int b=bit(rc,deep+node,(v>>i)&1);
            update(wide+node,b); node=node*2+b;
        }
        if (rc->decode) v=node-256;
        (*misses)++;
    }
    if (c) candidate_update(c,v); else *most=v+1;
    m->second[h2]=v+1; m->r=m->s; m->s=m->x; m->x=v;
    return v;
}
int zsx_stream_encode(const uint8_t *in, size_t n, unsigned profile,
                      uint8_t **out, size_t *out_size) {
    Model m;
    Coder c={0};
    uint32_t misses=0;
    int ret;
    uint8_t *buf;
    if (!out || !out_size || (!in && n) || n>CAP) return -1;
    ret=model_new(&m,profile);
    if (ret) return ret;
    buf=malloc(n+HEADER);
    if (!buf) { model_free(&m); return -2; }
    c.out=buf+HEADER; c.size=n; c.range=UINT32_MAX; c.pending=1;
    for (size_t i=0;i<n && !c.error;i++) step(&m,&c,in[i],&misses);
    for (int i=0;i<5 && !c.error;i++) carry(&c);
    model_free(&m);
    if (c.error || c.at>=n) {
        if (n) memcpy(buf+HEADER,in,n);
        c.at=n; misses=UINT32_MAX;
    }
    memcpy(buf,"ZSE1",4); put32(buf+4,profile);
    put32(buf+8,n); put32(buf+12,c.at); put32(buf+16,misses); put32(buf+20,1);
    *out=buf; *out_size=HEADER+c.at;
    return 0;
}
int zsx_stream_decode(const uint8_t *in, size_t n, uint8_t **out, size_t *out_size) {
    Model m;
    Coder c={0};
    uint32_t size, packed, misses, observed=0, profile;
    uint8_t *buf;
    int ret;
    if (!in || !out || !out_size || n<HEADER || n>CAP+HEADER ||
        memcmp(in,"ZSE1",4)) return -1;
    profile=get32(in+4); size=get32(in+8); packed=get32(in+12);
    misses=get32(in+16);
    if (profile<1 || profile>4 || size>CAP || packed!=n-HEADER ||
        get32(in+20)!=1 || (misses!=UINT32_MAX && misses>size)) return -1;
    if (misses==UINT32_MAX) {
        if (packed!=size) return -1;
    } else if (!size || packed<5 || packed>=size || in[HEADER]!=0) return -1;
    buf=malloc(size?size:1);
    if (!buf) return -2;
    if (misses==UINT32_MAX) {
        if (size) memcpy(buf,in+HEADER,size);
    } else {
        ret=model_new(&m,profile);
        if (ret) { free(buf); return ret; }
        c.decode=1; c.in=in+HEADER; c.size=packed; c.at=1; c.range=UINT32_MAX;
        for (int i=0;i<4;i++) c.code=(c.code<<8)|next(&c);
        for (size_t i=0;i<size && !c.error;i++) buf[i]=step(&m,&c,0,&observed);
        model_free(&m);
        if (c.error || c.code>=c.range || c.at!=packed || c.code!=0 || observed!=misses) {
            free(buf); return -1;
        }
    }
    *out=buf; *out_size=size;
    return 0;
}
