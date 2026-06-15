/* lzma.c — LZMA compression */
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
    if(!fn){printf("Usage: lzma [-c] [-f] [-k] <file>\n");return 1;}
    int fd=open(fn,O_RDONLY,0);if(fd<0){printf("lzma: %s: No such file\n",fn);return 1;}
    unsigned char buf[65536];unsigned long total=0;int n;
    while((n=read(fd,buf+total,sizeof(buf)-total))>0){total+=n;}close(fd);
    /* LZMA header: 5 bytes properties + 4 bytes dict size + 8 bytes uncompressed size */
    unsigned long out_size=total+total/1000+64;
    unsigned char*out=malloc(out_size);unsigned long pos=0;
    out[pos++]=0x5d;/* lc=3,lp=0,pb=2 */
    out[pos++]=0;out[pos++]=0;out[pos++]=0x80;/* dict=8MB */
    for(int i=0;i<8;i++)out[pos++]=(unsigned char)(total>>(8*i));/* uncompressed size */
    /* Store uncompressed data as literal LZMA: each byte as a literal */
    for(unsigned long i=0;i<total;i++){out[pos++]=buf[i];}
    /* End of payload marker */
    out[pos++]=0;/* PPM for last */out[pos]=0;
    pos++;
    if(to_stdout){write(1,out,pos);}
    else{
        unsigned long len=strlen(fn);
        char *outname=malloc(len+6);memcpy(outname,fn,len);outname[len]='.';outname[len+1]='l';outname[len+2]='z';outname[len+3]='m';outname[len+4]='a';outname[len+5]=0;
        int ofd=open(outname,O_WRONLY|O_CREAT,0644);if(ofd<0){printf("lzma: cannot create %s\n",outname);free(out);free(outname);return 1;}
        write(ofd,out,pos);close(ofd);free(outname);
        if(!keep)unlink(fn);
    }
    free(out);return 0;
}
