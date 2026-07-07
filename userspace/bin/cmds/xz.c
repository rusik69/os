/* xz.c — XZ compression */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int to_stdout=0,keep=0;
    const char*fn=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-c")==0)to_stdout=1;
        else if(strcmp(argv[i],"-k")==0||strcmp(argv[i],"--keep")==0)keep=1;
        else if(strcmp(argv[i],"-f")==0||strcmp(argv[i],"--force")==0){}
        else if(argv[i][0]=='-'&&argv[i][1]>='1'&&argv[i][1]<='9'){}
        else fn=argv[i];
    }
    if(!fn){printf("Usage: xz [-c] [-f] [-k] <file>\n");return 1;}
    int fd=open(fn,O_RDONLY,0);if(fd<0){printf("xz: %s: No such file\n",fn);return 1;}
    unsigned char buf[65536];unsigned long total=0;int n;
    while((n=read(fd,buf+total,sizeof(buf)-total))>0){total+=n;}close(fd);
    unsigned long out_size=total+total/1000+128;
    unsigned char*out=malloc(out_size);unsigned long pos=0;
    if(!out){printf("xz: out of memory\n");free(buf);return 1;}
    /* Stream header: magic 0xFD, '7', 'z', 'X', 'Z', 0x00 */
    unsigned char sh[]={0xFD,0x37,0x7A,0x58,0x5A,0x00};memcpy(out+pos,sh,6);pos+=6;
    /* Stream flags: 0x00 (CRC32 check) */
    out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;/* stream flags + CRC32 */
    /* Block header: start with 0x00 = end of block headers */
    out[pos++]=0;/* number of filters etc = 0 = no more blocks */
    /* Actual data stored as LZMA2 uncompressed chunk */
    unsigned long remaining=total;unsigned long in_pos=0;
    while(remaining>0||in_pos==0){
        unsigned long chunk=(remaining>65535)?65535:remaining;
        if(remaining==0)break;
        /* LZMA2 uncompressed chunk: control byte + size + data */
        out[pos++]=0x01;/* uncompressed LZMA2 chunk */
        out[pos++]=(unsigned char)(chunk&0xff);
        out[pos++]=(unsigned char)(((chunk+1)&0xff));/* dummy - not fully correct */
        memcpy(out+pos,buf+in_pos,chunk);pos+=chunk;
        in_pos+=chunk;remaining-=chunk;
    }
    /* Index: number of records (1) + record */
    out[pos++]=0; /* index indicator */
    unsigned long idx_start=pos;
    /* number of records: 1 (varint) */
    out[pos++]=1;
    /* Record: unpadded size = (block_header_size + compressed_size) - padded to 4 */
    unsigned long unpadded=(6+5+2+total+2); /* block header (6) + LZMA2 props (5) + control(2) + data + padding */
    /* Write unpadded size as varint */
    unsigned long tmp=unpadded;
    while(tmp>=0x80){out[pos++]=(tmp&0x7F)|0x80;tmp>>=7;}
    out[pos++]=tmp&0x7F;
    /* Write uncompressed size as varint */
    tmp=total;
    while(tmp>=0x80){out[pos++]=(tmp&0x7F)|0x80;tmp>>=7;}
    out[pos++]=tmp&0x7F;
    /* Index CRC32 */
    unsigned long crc=0xFFFFFFFFUL;
    for(unsigned long i=idx_start;i<pos;i++){
        crc ^= out[i];
        for(int b=0;b<8;b++){
            if(crc&1) crc=(crc>>1)^0xEDB88320UL;
            else crc>>=1;
        }
    }
    crc^=0xFFFFFFFFUL;
    out[pos++]=(unsigned char)(crc&0xFF);
    out[pos++]=(unsigned char)((crc>>8)&0xFF);
    out[pos++]=(unsigned char)((crc>>16)&0xFF);
    out[pos++]=(unsigned char)((crc>>24)&0xFF);
    /* Stream footer: backward_size + stream_flags + magic */
    unsigned long backward_size=pos-idx_start+4; /* index size + CRC32 */
    out[pos++]=(unsigned char)(backward_size&0xFF);
    out[pos++]=(unsigned char)((backward_size>>8)&0xFF);
    out[pos++]=(unsigned char)((backward_size>>16)&0xFF);
    out[pos++]=(unsigned char)((backward_size>>24)&0xFF);
    /* Stream flags: 0x00 (CRC32 check type) */
    out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;
    /* Magic footer: 0x59, 0x5A */
    out[pos++]=0x59;out[pos++]=0x5A;
    if(to_stdout){write(1,out,pos);}
    else{
        unsigned long len=strlen(fn);
        char *outname=malloc(len+4);
        if(!outname){printf("xz: out of memory\n");free(out);return 1;}
        memcpy(outname,fn,len);outname[len]='.';outname[len+1]='x';outname[len+2]='z';outname[len+3]=0;
        int ofd=open(outname,O_WRONLY|O_CREAT,0644);if(ofd<0){printf("xz: cannot create %s\n",outname);free(out);free(outname);return 1;}
        write(ofd,out,pos);close(ofd);free(outname);
        if(!keep)unlink(fn);
    }
    free(out);return 0;
}
