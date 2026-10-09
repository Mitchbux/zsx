/* Bundled authoritative algorithm. The generator prefixes the global CDF.
 * No includes or host prediction callbacks are needed by reconstruction. */
typedef unsigned U;
typedef unsigned char B;
typedef unsigned long long W;
static U lo,hi,cv,pending,bitpos,limit,bad,writing;
static B *packed;
static U next[3][65536],prob[3][65536],seen[3][65536],previous,older,history;
static W hits,events,wordhits,wordevents,letterhits,letterevents;
static W trained_lookups,trained_hits,trained_misses;
static U getbit(void){
    U v;
    if(bitpos>=limit){bad=1;return 0;}
    v=(packed[bitpos>>3]>>(7-(bitpos&7)))&1;bitpos++;return v;
}
static void putbit(U v){
    if(bitpos>=limit){bad=1;return;}
    if(v)packed[bitpos>>3]|=1u<<(7-(bitpos&7));
    bitpos++;
}
static void emit(U v){putbit(v);while(pending){putbit(!v);pending--;}}
static U target(U total){
    return (U)((((W)cv-lo+1)*total-1)/((W)hi-lo+1));
}
static void interval(U a,U b,U total){
    W range=(W)hi-lo+1;
    hi=lo+(U)(range*b/total)-1;lo+= (U)(range*a/total);
    for(;;){
        if(hi<0x80000000u){if(writing)emit(0);}
        else if(lo>=0x80000000u){
            if(writing)emit(1);
            lo-=0x80000000u;hi-=0x80000000u;cv-=0x80000000u;
        }else if(lo>=0x40000000u&&hi<0xc0000000u){
            if(writing)pending++;
            lo-=0x40000000u;hi-=0x40000000u;cv-=0x40000000u;
        }else break;
        lo<<=1;hi=(hi<<1)|1;
        if(!writing)cv=(cv<<1)|getbit();
        if(bad)break;
    }
}
static U binary(U value,U chance){
    if(!writing)value=target(4096)>=chance;
    interval(value?chance:0,value?4096:chance,4096);
    return value;
}
/* Shared global-frequency lookup is used after all context predictions miss.
 * next[] learns next word/letter tokens at orders 1, 2 and 3; probabilities
 * adapt independently of the global word/letter CDF prefixed by the generator. */
static U symbol(U actual){
    U contexts[3],prediction,miss=1,s=actual,left,right,middle,level,k,entry=CTRAIN;
    /* Offline corpus-trained prior-two-token top-four distribution. All
     * entries live in counted archived C, never in another residual stream. */
    if(CTRAIN){
        U key=older*N+previous,a=0,b=CTRAIN;
        while(a<b){
            U mid=(a+b)/2;
            if(trained[mid][0]<key)a=mid+1;else b=mid;
        }
        if(a<CTRAIN&&trained[a][0]==key){
            U total=trained[a][9]+5,cumulative=0,value=0,targetvalue=0;
            entry=a;trained_lookups++;
            if(!writing)targetvalue=target(total);
            for(k=0;k<4;k++){
                value=trained[a][2+2*k]+1;
                if(writing?actual==trained[a][1+2*k]:targetvalue<cumulative+value){
                    s=trained[a][1+2*k];miss=0;break;
                }
                cumulative+=value;
            }
            interval(cumulative,k<4?cumulative+value:total,total);
            if(!miss){trained_hits++;hits++;}else trained_misses++;
        }
    }
    contexts[0]=previous;
    contexts[1]=(previous*257u+older)&65535;
    contexts[2]=(previous*8191u+older*31u+history)&65535;
    for(level=ORDER;miss&&level>0;level--){
        k=level-1;
        if(!seen[k][contexts[k]])continue;
        prediction=next[k][contexts[k]];
        if(entry<CTRAIN){
            U j;
            for(j=0;j<4;j++)if(prediction==trained[entry][1+2*j])break;
            if(j<4)continue; /* Already excluded by the trained escape. */
        }
        miss=binary(actual!=prediction,prob[k][contexts[k]]);
        if(!miss){s=prediction;hits++;break;}
    }
    if(miss&&writing){s=actual;interval(cdf[s],cdf[s+1],cdf[N]);}
    else if(miss) {
        U t=target(cdf[N]);left=0;right=N;
        while(left+1<right){
            middle=(left+right)/2;
            if(cdf[middle]<=t)left=middle;else right=middle;
        }
        s=left;interval(cdf[s],cdf[s+1],cdf[N]);
    }
    events++;
    if(s>=256){wordevents++;if(!miss)wordhits++;}
    else {letterevents++;if(!miss)letterhits++;}
    for(k=0;k<ORDER;k++){
        U context=contexts[k],p=prob[k][context];
        if(next[k][context]!=s)p-=(p+31)/32;
        else p+=(4096-p+31)/32;
        if(p<1)p=1;
        if(p>4095)p=4095;
        prob[k][context]=p;seen[k][context]=1;next[k][context]=s;
    }
    history=older;older=previous;previous=s;return s;
}
static void initialize(B *data,U bytes,U encode){
    U i,k;packed=data;limit=bytes*8;bitpos=bad=pending=cv=lo=previous=older=history=0;
    hi=~0u;writing=encode;hits=events=wordhits=wordevents=letterhits=letterevents=0;
    trained_lookups=trained_hits=trained_misses=0;
    for(k=0;k<ORDER;k++)for(i=0;i<65536;i++){next[k][i]=TOP;prob[k][i]=2048;seen[k][i]=0;}
    if(!writing)for(i=0;i<32;i++)cv=(cv<<1)|getbit();
}
/* Case is interleaved in the SAME arithmetic residual: lower/title/upper/mixed.
 * The common lowercase outcome has a strong prior, adapted per word. */
static U caseprob[2];
static U casebit(U v,U index){
    U q=binary(v,caseprob[index]);
    if(q)caseprob[index]-=(caseprob[index]+31)/32;
    else caseprob[index]+=(4096-caseprob[index]+31)/32;
    if(caseprob[index]<1)caseprob[index]=1;
    if(caseprob[index]>4095)caseprob[index]=4095;
    return q;
}
int reconstruct(B *residual,U bytes,B *dictionary,U dictbytes,B *out,U wanted){
    U offsets[N],lengths[N],at=0,i,j,s,len,pos=0,mode;
    if(bytes<4||bytes>200000000u)return 0;
    /* Basic generated-model bounds; these are not a sandbox for arbitrary C. */
    if(N<256||N>8448||CTRAIN>65536)return 0;
    if(cdf[0]||cdf[N]>2000000u)return 0;
    for(i=0;i<N;i++)if(cdf[i]>=cdf[i+1])return 0;
    for(i=0;i<CTRAIN;i++){
        U sum=0;
        if(trained[i][0]>=N*N||(i&&trained[i-1][0]>=trained[i][0])||
           trained[i][9]>100000000u)return 0;
        for(j=0;j<4;j++){
            U q;
            if(trained[i][1+2*j]>=N||trained[i][2+2*j]>trained[i][9])return 0;
            for(q=0;q<j;q++)if(trained[i][1+2*q]==trained[i][1+2*j])return 0;
            sum+=trained[i][2+2*j];
        }
        if(sum>trained[i][9])return 0;
    }
    for(i=256;i<N;i++){
        if(at>=dictbytes)return 0;
        len=dictionary[at++];if(!len||len>dictbytes-at)return 0;
        offsets[i]=at;lengths[i]=len;
        for(j=0;j<len;j++)if(dictionary[at+j]<'a'||dictionary[at+j]>'z')return 0;
        at+=len;
    }
    if(at!=dictbytes)return 0;
    initialize(residual,bytes,0);caseprob[0]=3800;caseprob[1]=2048;
    while(pos<wanted&&!bad){
        s=symbol(0);
        if(s<256){out[pos++]=(B)s;continue;}
        len=lengths[s];if(len>wanted-pos)return 0;
        mode=casebit(0,0);
        if(mode){mode=1+casebit(0,1);mode+=binary(0,2048)<<1;}
        if(mode>3)return 0;
        for(j=0;j<len;j++){
            U c=dictionary[offsets[s]+j];
            if(mode==2||(mode==1&&!j)||(mode==3&&binary(0,2048)))c-=32;
            out[pos++]=(B)c;
        }
    }
    /* Arithmetic finish has at most 40 unused padding/lookahead bits.
     * This is structural validation, not an integrity check. */
    return !bad&&pos==wanted&&limit-bitpos<=40;
}