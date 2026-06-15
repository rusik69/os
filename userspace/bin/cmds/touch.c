/* touch.c — create/update file timestamps */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: touch <file>...\n");return 1;}
    for(int i=1;i<argc;i++){
        int fd=open(argv[i],O_CREAT|O_WRONLY,0666);
        if(fd<0){printf("touch: cannot create '%s'\n",argv[i]);return 1;}
        close(fd);
    }
    return 0;
}
