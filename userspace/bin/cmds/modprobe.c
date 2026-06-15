/* modprobe.c — load module with dependency resolution */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: modprobe <module>\n");return 1;}
    if(init_module(argv[1],"")<0&&query_module(argv[1],0,0)<0)
        printf("modprobe: cannot load %s\n",argv[1]);
    return 0;
}
