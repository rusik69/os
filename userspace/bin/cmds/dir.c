/* dir.c — list directory contents */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    const char*dir=argc>1?argv[1]:".";
    int fd=open(dir,O_RDONLY,0);
    if(fd<0){printf("dir: %s: No such directory\n",dir);return 1;}
    printf("Directory: %s\n",dir);
    close(fd);
    return 0;
}
