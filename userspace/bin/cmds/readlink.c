/* readlink.c — print resolved symbolic link */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: readlink <file>...\n");return 1;}
    for(int i=1;i<argc;i++){
        char buf[4096];
        int n=readlink(argv[i],buf,sizeof(buf)-1);
        if(n<0){printf("readlink: %s: not a symlink\n",argv[i]);return 1;}
        buf[n]=0;write(1,buf,n);write(1,"\n",1);
    }
    return 0;
}
