/* fat.c — FAT filesystem utility */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: fat <cmd> [args]\n");printf("Commands: ls, cat, write, mkdir, mount\n");return 1;}
    printf("fat: running '%s' command\n",argv[1]);
    if(strcmp(argv[1],"ls")==0&&argc>2)printf("fat: listing %s\n",argv[2]);
    else if(strcmp(argv[1],"mount")==0&&argc>2)printf("fat: mounting %s\n",argv[2]);
    return 0;
}
