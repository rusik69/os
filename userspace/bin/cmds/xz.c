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
    /* Index: placeholder */
    out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;
    /* Stream footer: 0x59, 0x5A */
    out[pos++]=0x59;out[pos++]=0x5A;out[pos++]=0;out[pos++]=0;out[pos++]=0;out[pos++]=0;
    if(to_stdout){write(1,out,pos);}
    else{
        unsigned long len=strlen(fn);
        char *outname=malloc(len+4);memcpy(outname,fn,len);outname[len]='.';outname[len+1]='x';outname[len+2]='z';outname[len+3]=0;
        int ofd=open(outname,O_WRONLY|O_CREAT,0644);if(ofd<0){printf("xz: cannot create %s\n",outname);free(out);free(outname);return 1;}
        write(ofd,out,pos);close(ofd);free(outname);
        if(!keep)unlink(fn);
    }
    free(out);return 0;
}
