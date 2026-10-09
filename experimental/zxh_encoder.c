/* Native generator harness appended to exactly the archived decoder source.
 * This harness is NOT archived and is never used during reconstruction. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static B *load(const char *name,U *n){
    FILE *f=fopen(name,"rb");long z;B *p;
    if(!f||fseek(f,0,SEEK_END)||(z=ftell(f))<0||z>100000000||fseek(f,0,SEEK_SET))exit(2);
    *n=(U)z;p=calloc((size_t)*n+1,1);
    if(!p||fread(p,1,*n,f)!=*n||fclose(f))exit(2);
    return p;
}
static U alpha(U c){return (c>=65&&c<=90)||(c>=97&&c<=122);}
static U lower(U c){return c>=65&&c<=90?c+32:c;}
static U hash(B *p,U n){U h=2166136261u;while(n--)h=(h^lower(*p++))*16777619u;return h;}
static int training;
static W *training_events;
static U training_count;
static void encode_symbol(U s){
    if(!training){symbol(s);return;}
    training_events[training_count++]=((W)older*N+previous)*N+s;
    older=previous;previous=s;
}
typedef struct {U key,tokens[4],counts[4],total;} Trained;
static int compare_event(const void *a,const void *b){
    W x=*(const W *)a,y=*(const W *)b;return (x>y)-(x<y);
}
static int compare_popularity(const void *a,const void *b){
    const Trained *x=a,*y=b;
    if(x->total!=y->total)return (x->total<y->total)-(x->total>y->total);
    return (x->key>y->key)-(x->key<y->key);
}
static int compare_context(const void *a,const void *b){
    const Trained *x=a,*y=b;return (x->key>y->key)-(x->key<y->key);
}
static int save_training(const char *path){
    U at=0,rows=0,i,j,k;FILE *f;
    Trained *records=calloc((size_t)training_count/64+1,sizeof(*records));
    if(!records)return 2;
    qsort(training_events,training_count,sizeof(W),compare_event);
    while(at<training_count){
        Trained r={0};r.key=(U)(training_events[at]/N);
        while(at<training_count&&training_events[at]/N==r.key){
            U token=(U)(training_events[at]%N),count=0;
            while(at<training_count&&training_events[at]==(W)r.key*N+token){at++;count++;}
            r.total+=count;
            for(j=0;j<4;j++)if(count>r.counts[j]){
                for(k=3;k>j;k--){r.counts[k]=r.counts[k-1];r.tokens[k]=r.tokens[k-1];}
                r.counts[j]=count;r.tokens[j]=token;break;
            }
        }
        /* Fill missing zero-count ranks with distinct legal token IDs. */
        for(j=0;j<4;j++)if(!r.counts[j]){
            for(i=0;i<N;i++){
                for(k=0;k<j;k++)if(r.tokens[k]==i)break;
                if(k==j){r.tokens[j]=i;break;}
            }
        }
        if(r.total>=64)records[rows++]=r;
    }
    free(training_events);
    qsort(records,rows,sizeof(*records),compare_popularity);
    if(rows>65536)rows=65536;
    qsort(records,rows,sizeof(*records),compare_context);
    f=fopen(path,"wb");if(!f)return 2;
    fprintf(f,"#define CTRAIN %u\nstatic unsigned trained[%u][10]={\n",rows,rows?rows:1);
    if(!rows)fprintf(f,"{0}");
    for(i=0;i<rows;i++){
        Trained *r=records+i;
        fprintf(f,"{%u",r->key);
        for(j=0;j<4;j++)fprintf(f,",%u,%u",r->tokens[j],r->counts[j]);
        fprintf(f,",%u},\n",r->total);
    }
    fprintf(f,"};\n");free(records);return fclose(f)?2:0;
}
int main(int argc,char **argv){
    U n,dn,i,j,k,at=0,pos=0,start,len,s,mode,upper;
    U table[32768]={0},offset[N],length[N];
    B *input,*dict,*residual;FILE *f;
    if(argc!=4&&(argc!=5||strcmp(argv[4],"--train")))return 2;
    training=argc==5;
    input=load(argv[1],&n);dict=load(argv[2],&dn);
    for(i=256;i<N;i++){
        if(at>=dn)return 2;len=dict[at++];if(!len||len>dn-at)return 2;
        offset[i]=at;length[i]=len;
        k=hash(dict+at,len)&32767;while(table[k])k=(k+1)&32767;table[k]=i;at+=len;
    }
    if(at!=dn)return 2;
    residual=NULL;
    if(training){training_events=malloc(((size_t)n+1)*sizeof(W));if(!training_events)return 2;}
    else {
        residual=calloc((size_t)n*4+1024,1);if(!residual)return 2;
        initialize(residual,n*4+1024,1);caseprob[0]=3800;caseprob[1]=2048;
    }
    while(pos<n&&!bad){
        start=pos;len=0;s=0;
        /* A maximal ASCII word is either a dedicated dictionary token or
         * escaped unchanged as bytes, including binary/UTF-8 bytes. */
        if(alpha(input[pos])){
            while(pos+len<n&&alpha(input[pos+len]))len++;
            k=hash(input+pos,len)&32767;
            while(table[k]){
                i=table[k];
                if(length[i]==len){
                    for(j=0;j<len&&lower(input[pos+j])==dict[offset[i]+j];j++);
                    if(j==len){s=i;break;}
                }
                k=(k+1)&32767;
            }
        }
        if(!s){
            if(!len)len=1;
            for(j=0;j<len;j++)encode_symbol(input[pos++]);
            continue;
        }
        encode_symbol(s);
        if(training){pos=start+len;continue;}
        upper=0;
        for(j=0;j<len;j++)upper+=input[pos+j]<=90;
        mode=!upper?0:upper==len?2:upper==1&&input[pos]<=90?1:3;
        casebit(mode!=0,0);
        if(mode){casebit((mode-1)&1,1);binary((mode-1)>>1,2048);}
        for(j=0;j<len;j++)if(mode==3)binary(input[pos+j]<=90,2048);
        pos=start+len;
    }
    if(training){free(input);free(dict);return save_training(argv[3]);}
    pending++;emit(lo>=0x40000000u);
    for(i=0;i<32;i++)putbit(0);
    if(bad)return 2;
    f=fopen(argv[3],"wb");
    if(!f||fwrite(residual,1,(bitpos+7)/8,f)!=(bitpos+7)/8||fclose(f))return 2;
    printf("{\"events\":%llu,\"prediction_hits\":%llu,\"word_events\":%llu,"
           "\"word_hits\":%llu,\"letter_events\":%llu,\"letter_hits\":%llu,"
           "\"trained_lookups\":%llu,\"trained_hits\":%llu,\"trained_misses\":%llu}\n",
           events,hits,wordevents,wordhits,letterevents,letterhits,
           trained_lookups,trained_hits,trained_misses);
    free(input);free(dict);free(residual);return 0;
}