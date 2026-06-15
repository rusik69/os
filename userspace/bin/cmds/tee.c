/* tee.c — read and write to files and stdout */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    int append=0,i=1,outfds[16],nout=0;
    if(argc>1&&strcmp(argv[1],"-a")==0){append=1;i=2;}
    for(;i<argc&&nout<16;i++){
        outfds[nout]=open(argv[i],O_WRONLY|O_CREAT|(append?O_APPEND:O_TRUNC),0666);
        if(outfds[nout]>=0)nout++;
    }
    char buf[4096];long n;
    while((n=read(0,buf,sizeof(buf)))>0){
        write(1,buf,n);
        for(int j=0;j<nout;j++)write(outfds[j],buf,n);
    }
    for(int j=0;j<nout;j++)close(outfds[j]);
    return 0;
}
