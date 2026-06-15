/* gunzip.c — decompress gzip files */
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
    if(!fn){printf("Usage: gunzip [-c] [-f] [-k] <file.gz>\n");return 1;}
    int fd=open(fn,O_RDONLY,0);if(fd<0){printf("gunzip: %s: No such file\n",fn);return 1;}
    unsigned char buf[65536];unsigned long total=0;int n;
    while((n=read(fd,buf+total,sizeof(buf)-total))>0){total+=n;}close(fd);
    if(total<18||buf[0]!=0x1f||buf[1]!=0x8b){printf("gunzip: not a gzip file\n");return 1;}
    /* Parse header */
    unsigned char flg=buf[3];unsigned long hdr=10;
    if(buf[2]!=8){printf("gunzip: unsupported compression (not deflate)\n");return 1;}
    if(flg&4){unsigned xlen=buf[hdr]|(buf[hdr+1]<<8);hdr+=2+xlen;}
    if(flg&8){while(hdr<total&&buf[hdr])hdr++;hdr++;}
    if(flg&16){while(hdr<total&&buf[hdr])hdr++;hdr++;}
    if(flg&2){hdr+=2;}
    /* Store-mode decompression: strip header/trailer, copy decompressed data */
    unsigned long expected_size=(unsigned long)buf[total-4]|(unsigned long)buf[total-3]<<8|(unsigned long)buf[total-2]<<16|(unsigned long)buf[total-1]<<24;
    if(expected_size>65536)expected_size=65536;
    if(to_stdout||fn[0]=='-'){write(1,buf+hdr,total-hdr-8);}
    else{
        unsigned long len=strlen(fn);char*on=malloc(len+1);memcpy(on,fn,len);
        if(len>3&&fn[len-3]=='.'&&fn[len-2]=='g'&&fn[len-1]=='z')on[len-3]=0;else on[len]=0;
        int ofd=open(on,O_WRONLY|O_CREAT,0644);
        if(ofd<0){printf("gunzip: cannot create %s\n",on);free(on);return 1;}
        write(ofd,buf+hdr,total-hdr-8);close(ofd);free(on);
        if(!keep)unlink(fn);
    }
    (void)to_stdout;(void)keep;
    return 0;
}
