/* rmmod.c — unload kernel module */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: rmmod <module>\n");return 1;}
    if(delete_module(argv[1],0)<0){printf("rmmod: cannot remove %s\n",argv[1]);return 1;}
    return 0;
}
