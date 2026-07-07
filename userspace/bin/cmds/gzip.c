/* gzip.c — compress files (gzip format, stored blocks) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static unsigned long crc32_tab[256];
static void crc32_init(void){
    static int done=0;if(done)return;done=1;
    for(unsigned i=0;i<256;i++){unsigned long c=i;for(int j=0;j<8;j++)c=(c>>1)^(c&1?0xedb88320UL:0);crc32_tab[i]=c;}
}
static unsigned long crc32(const unsigned char*data,unsigned long len,unsigned long crc){
    crc=~crc;for(unsigned long i=0;i<len;i++)crc=crc32_tab[(unsigned char)(crc^data[i])]^(crc>>8);
    return ~crc;
}

int main(int argc,char*argv[]){
    int to_stdout=0,keep=0,level=6;
    const char*fn=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-c")==0)to_stdout=1;
        else if(strcmp(argv[i],"-k")==0||strcmp(argv[i],"--keep")==0)keep=1;
        else if(strcmp(argv[i],"-f")==0||strcmp(argv[i],"--force")==0){}
        else if(argv[i][0]=='-'&&argv[i][1]>='1'&&argv[i][1]<='9')level=argv[i][1]-'0';
        else fn=argv[i];
    }
    if(!fn){printf("Usage: gzip [-c] [-f] [-k] [-1..-9] <file>\n");return 1;}
    int fd=open(fn,O_RDONLY,0);if(fd<0){printf("gzip: %s: No such file\n",fn);return 1;}
    unsigned char buf[65536];unsigned long total=0;int n;
    while((n=read(fd,buf+total,sizeof(buf)-total))>0){total+=n;}close(fd);
    (void)level;
    /* Build output */
    unsigned long out_size=total+total/1000+64;
    unsigned char*out=malloc(out_size);unsigned long pos=0;
    if(!out){printf("gzip: out of memory\n");free(buf);return 1;}
    /* Gzip header */
    out[pos++]=0x1f;out[pos++]=0x8b;out[pos++]=8;/* deflate */
    out[pos++]=0;/* flags */out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;/* mtime */
    out[pos++]=0;/* xfl */out[pos++]=255;/* os (unknown) */
    /* Deflate: stored (non-compressed) blocks */
    unsigned long remaining=total;unsigned long in_pos=0;
    while(remaining>0){
        unsigned long chunk=(remaining>65535)?65535:remaining;
        out[pos++]=(remaining==chunk)?1:0;
        out[pos++]=0;out[pos++]=0;
        out[pos++]=(unsigned char)(chunk&0xff);
        out[pos++]=(unsigned char)((chunk>>8)&0xff);
        out[pos++]=(unsigned char)((~chunk)&0xff);
        out[pos++]=(unsigned char)(((~chunk)>>8)&0xff);
        memcpy(out+pos,buf+in_pos,chunk);pos+=chunk;
        in_pos+=chunk;remaining-=chunk;
    }
    /* CRC32 and ISIZE trailer */
    crc32_init();unsigned long csum=crc32(buf,total,0);
    out[pos++]=(unsigned char)(csum&0xff);out[pos++]=(unsigned char)((csum>>8)&0xff);
    out[pos++]=(unsigned char)((csum>>16)&0xff);out[pos++]=(unsigned char)((csum>>24)&0xff);
    out[pos++]=(unsigned char)(total&0xff);out[pos++]=(unsigned char)((total>>8)&0xff);
    out[pos++]=(unsigned char)((total>>16)&0xff);out[pos++]=(unsigned char)((total>>24)&0xff);
    if(to_stdout){write(1,out,pos);}
    else{
        unsigned long len=strlen(fn);
        char *outname=malloc(len+4);
        if(!outname){printf("gzip: out of memory\n");free(out);return 1;}
        memcpy(outname,fn,len);outname[len]='.';outname[len+1]='g';outname[len+2]='z';outname[len+3]=0;
        int ofd=open(outname,O_WRONLY|O_CREAT,0644);if(ofd<0){printf("gzip: cannot create %s\n",outname);free(out);free(outname);return 1;}
        write(ofd,out,pos);close(ofd);free(outname);
        if(!keep)unlink(fn);
    }
    free(out);return 0;
}
