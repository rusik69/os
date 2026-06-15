/* insmod.c — load kernel module */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: insmod <module.ko>\n");return 1;}
    if(init_module(argv[1],"")<0){printf("insmod: cannot load %s\n",argv[1]);return 1;}
    return 0;
}
