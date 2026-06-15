/* dd.c — convert and copy a file */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    unsigned long block=512,count=0;
    char *ifile=0,*ofile=0;
    for(int i=1;i<argc;i++){
        if(strncmp(argv[i],"bs=",3)==0)block=atoi(argv[i]+3);
        else if(strncmp(argv[i],"count=",6)==0)count=atoi(argv[i]+6);
        else if(strncmp(argv[i],"if=",3)==0)ifile=argv[i]+3;
        else if(strncmp(argv[i],"of=",3)==0)ofile=argv[i]+3;
    }
    int ifd=ifile?open(ifile,O_RDONLY,0):0;
    int ofd=ofile?open(ofile,O_WRONLY|O_CREAT,0666):1;
    if(ifd<0){printf("dd: cannot open input\n");return 1;}
    if(ofd<0){printf("dd: cannot open output\n");return 1;}
    char buf[8192];unsigned long total=0,c=0;
    long n;
    while((n=read(ifd,buf,block>sizeof(buf)?sizeof(buf):block))>0){
        write(ofd,buf,n);total+=n;c++;
        if(count>0&&c>=count)break;
    }
    if(ifile)close(ifd);
    if(ofile)close(ofd);
    printf("%lu+0 records in\n%lu+0 records out\n%lu bytes copied\n",c,c,total);
    return 0;
}
