/* ZXH1 trusted generated-code experiment. Included only by native zsx.c.
 * Host responsibilities: framing, ordinary ZSX decompression, buffers, libtcc,
 * atomic output. Prediction/entropy/case logic exists ONLY in archived C. */
#ifdef ZSX_WITH_TCC
#include <libtcc.h>
static byte *zxh_section(FILE *f,uint32_t raw,uint32_t stored,uint32_t mode){
    byte *result=NULL,*data=NULL,*chunk=NULL;size_t at=16,pos=0;
    if(mode>1||(!mode&&raw!=stored)||stored>200000000u)return NULL;
    result=(byte *)calloc((size_t)raw+1,1);
    data=(byte *)malloc((size_t)stored+1);
    if(!result||!data||!read_all(f,data,stored))goto fail;
    if(!mode){memcpy(result,data,raw);free(data);return result;}
    if(stored<16||memcmp(data,"ZSX1",4)||get_u32(data+4)!=raw||
       get_u32(data+12)||get_u32(data+8)!=(raw+zsx_chunk-1)/zsx_chunk)goto fail;
    chunk=zsx_new(byte,(size_t)zsx_chunk+header_sizes+1);
    for(uint32_t i=0;i<get_u32(data+8);i++){
        if(stored-at<4)goto fail;
        uint32_t n=get_u32(data+at);at+=4;
        uint32_t expected=raw-(uint32_t)pos;
        if(expected>zsx_chunk)expected=zsx_chunk;
        if(n<header_sizes||n>zsx_chunk+header_sizes||n>stored-at)goto fail;
        memcpy(chunk,data+at,n);at+=n;zsx_len(chunk)=n;
        if(get_u32(chunk)!=expected||!zsx_decode(chunk)||(uint32_t)zsx_len(chunk)!=expected)goto fail;
        memcpy(result+pos,chunk,expected);pos+=expected;
    }
    if(at!=stored||pos!=raw)goto fail;
    zsx_del(chunk);free(data);return result;
fail:
    if(chunk)zsx_del(chunk);
    free(data);free(result);return NULL;
}
#endif
static int execute_zxh(const char *input_path,const char *output_path){
#ifndef ZSX_WITH_TCC
    (void)input_path;(void)output_path;
    fprintf(stderr,"zsx: ZXH1 execution unsupported in this build; rebuild WITH_TCC=1 (see README)\n");
    return 0;
#else
    FILE *f=NULL,*output=NULL;char *temporary=NULL;
    byte header[44],*parts[3]={NULL,NULL,NULL},*out=NULL;
    uint32_t sizes[3]={0};TCCState *t=NULL;int ok=0;
    int (*fn)(byte *,unsigned,byte *,unsigned,byte *,unsigned)=NULL;
    if(paths_alias(input_path,output_path)){
        fprintf(stderr,"zsx: input and output refer to the same file\n");return 0;
    }
    f=fopen(input_path,"rb");
    if(!f||!read_all(f,header,sizeof(header))||memcmp(header,"ZXH1",4)){
        fprintf(stderr,"zsx: invalid/truncated ZXH1 header\n");goto done;
    }
    uint32_t wanted=get_u32(header+4);
    if(wanted>100000000u){fprintf(stderr,"zsx: ZXH1 output bound exceeded\n");goto done;}
    for(unsigned i=0;i<3;i++){
        sizes[i]=get_u32(header+8+12*i);
        uint32_t stored=get_u32(header+12+12*i),mode=get_u32(header+16+12*i);
        if(sizes[i]>(i==0?4194304u:i==1?65536u:200000000u)||
           (i==0&&(!sizes[i]||stored>1048576u))||
           !(parts[i]=zxh_section(f,sizes[i],stored,mode))){
            fprintf(stderr,"zsx: invalid/truncated ZXH1 section %u\n",i);goto done;
        }
    }
    if(fgetc(f)!=EOF||ferror(f)||memchr(parts[0],0,sizes[0])){
        fprintf(stderr,"zsx: trailing data or invalid source\n");goto done;
    }
    fclose(f);f=NULL;
    out=(byte *)malloc((size_t)wanted+1);t=tcc_new();
    if(!out||!t){fprintf(stderr,"zsx: ZXH1 allocation failed\n");goto done;}
    tcc_set_options(t,"-nostdinc -nostdlib");
    if(tcc_set_output_type(t,TCC_OUTPUT_MEMORY)||tcc_compile_string(t,(char *)parts[0])){
        fprintf(stderr,"zsx: libtcc compilation failed\n");goto done;
    }
    /* TCC_RELOCATE_AUTO exists only in the older two-argument public ABI.
     * Use the installed header, never a guessed function declaration. */
#ifdef TCC_RELOCATE_AUTO
    if(tcc_relocate(t,TCC_RELOCATE_AUTO)){
#else
    if(tcc_relocate(t)){
#endif
        fprintf(stderr,"zsx: libtcc relocation failed\n");goto done;
    }
    void *symbol=tcc_get_symbol(t,"reconstruct");
    if(sizeof(fn)!=sizeof(symbol)){fprintf(stderr,"zsx: unsupported function pointer ABI\n");goto done;}
    memcpy(&fn,&symbol,sizeof(fn));
    if(!fn||!fn(parts[2],sizes[2],parts[1],sizes[1],out,wanted)){
        fprintf(stderr,"zsx: generated reconstruction structural/buffer failure\n");goto done;
    }
    output=create_temp_output(output_path,&temporary);
    if(!output){fprintf(stderr,"zsx: cannot create ZXH1 temporary output\n");goto done;}
    ok=write_all(output,out,wanted);
    if(fclose(output))ok=0;
    output=NULL;
    if(ok)ok=replace_output(temporary,output_path);
    if(!ok)fprintf(stderr,"zsx: ZXH1 output write/replace failed\n");
done:
    if(f)fclose(f);
    if(output)fclose(output);
    if(temporary){if(!ok)remove(temporary);free(temporary);}
    if(t)tcc_delete(t);
    free(out);for(unsigned i=0;i<3;i++)free(parts[i]);
    return ok;
#endif
}