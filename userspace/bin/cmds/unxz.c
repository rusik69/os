/* unxz.c — XZ decompression (simplified) */
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
        else fn=argv[i];
    }
    if(!fn){printf("Usage: unxz [-c] [-f] [-k] <file.xz>\n");return 1;}
    int fd=open(fn,O_RDONLY,0);if(fd<0){printf("unxz: %s: No such file\n",fn);return 1;}
    unsigned char buf[65536];unsigned long total=0;int n;
    while((n=read(fd,buf+total,sizeof(buf)-total))>0){total+=n;}close(fd);
    if(total<12||buf[0]!=0xFD||buf[1]!=0x37||buf[2]!=0x7A||buf[3]!=0x58||buf[4]!=0x5A){printf("unxz: not an XZ file\n");return 1;}
    /* Very simplified: look for uncompressed LZMA2 data after header */
    unsigned long pos=6;/* skip header */
    /* Skip stream flags */
    pos+=5;/* stream flags + CRC32 */
    /* Parse blocks: skip block header (variable), extract LZMA2 data */
    unsigned long out_size=total*4;/* generous */
    unsigned char*out=malloc(out_size);unsigned long out_pos=0;
    unsigned long max_out=out_size;
    while(pos<total){
        if(buf[pos]==0){pos++;break;}/* end of block headers */
        /* Block header: skip variable length */
        unsigned long hdr_len=buf[pos]+1;
        pos+=hdr_len;
        if(pos>=total)break;
        /* Extract LZMA2 data: uncompressed chunks */
        while(pos<total){
            if(buf[pos]==0x00){pos++;break;}/* end of LZMA2 data */
            if(buf[pos]>=0x01&&buf[pos]<=0x7F){
                /* Uncompressed chunk */
                unsigned ctrl=buf[pos++];
                unsigned long chunk_len=(unsigned)ctrl<<16|(unsigned)buf[pos]<<8|(unsigned)buf[pos+1];
                pos+=2;
                if(pos+chunk_len>total)chunk_len=total-pos;
                if(out_pos+chunk_len>max_out)break;
                memcpy(out+out_pos,buf+pos,chunk_len);out_pos+=chunk_len;pos+=chunk_len;
            }else{pos++;break;}
        }
    }
    if(to_stdout){write(1,out,out_pos);}
    else{
        unsigned long len=strlen(fn);
        char *outname=malloc(len+1);memcpy(outname,fn,len);
        if(len>3&&fn[len-3]=='.'&&fn[len-2]=='x'&&fn[len-1]=='z')outname[len-3]=0;
        else{outname[len]=0;}
        int ofd=open(outname,O_WRONLY|O_CREAT,0644);
        if(ofd<0){printf("unxz: cannot create %s\n",outname);free(out);free(outname);return 1;}
        write(ofd,out,out_pos);close(ofd);free(outname);
        if(!keep)unlink(fn);
    }
    free(out);return 0;
}
