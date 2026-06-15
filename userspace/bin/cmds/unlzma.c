/* unlzma.c — LZMA decompression */
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
    if(!fn){printf("Usage: unlzma [-c] [-f] [-k] <file.lzma>\n");return 1;}
    int fd=open(fn,O_RDONLY,0);if(fd<0){printf("unlzma: %s: No such file\n",fn);return 1;}
    unsigned char buf[65536];unsigned long total=0;int n;
    while((n=read(fd,buf+total,sizeof(buf)-total))>0){total+=n;}close(fd);
    if(total<13){printf("unlzma: truncated file\n");return 1;}
    unsigned long uncomp_size=0;
    for(int i=0;i<8;i++)uncomp_size|=(unsigned long)buf[9+i]<<(8*i);
    unsigned long pos=17;/* skip header */
    /* Uncompressed LZMA stores literals and a single EOP marker */
    unsigned long out_pos=0;
    unsigned char*out=malloc(uncomp_size+1);
    if(!out){printf("unlzma: out of memory\n");return 1;}
    while(pos<total&&out_pos<uncomp_size){out[out_pos++]=buf[pos++];}
    if(to_stdout||(argc>1&&strcmp(argv[1],"-c")==0)){write(1,out,out_pos);}
    else{
        unsigned long len=strlen(fn);
        char *outname=malloc(len+1);memcpy(outname,fn,len);
        if(len>5&&fn[len-5]=='.'&&fn[len-4]=='l'&&fn[len-3]=='z'&&fn[len-2]=='m'&&fn[len-1]=='a')
            outname[len-5]=0;
        else{outname[len]=0;}
        int ofd=open(outname,O_WRONLY|O_CREAT,0644);
        if(ofd<0){printf("unlzma: cannot create %s\n",outname);free(out);free(outname);return 1;}
        write(ofd,out,out_pos);close(ofd);free(outname);
        if(!keep)unlink(fn);
    }
    free(out);return 0;
}
