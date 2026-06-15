/* mv.c — move/rename file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<3){printf("Usage: mv <source> <dest>\n");return 1;}
    if(rename(argv[1],argv[2])<0){printf("mv: failed\n");return 1;}
    return 0;
}
