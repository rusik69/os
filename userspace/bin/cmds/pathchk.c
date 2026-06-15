/* pathchk.c — check path validity */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: pathchk <path>...\n");return 1;}
    for(int i=1;i<argc;i++){
        if(strlen(argv[i])>4096){printf("pathchk: %s: path too long\n",argv[i]);return 1;}
        printf("%s: OK\n",argv[i]);
    }
    return 0;
}
