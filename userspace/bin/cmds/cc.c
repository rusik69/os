/* cc.c — invoke kernel C compiler */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: cc [options] <file>\n");return 1;}
    printf("cc: kernel C compiler available via kernel shell 'cc' command\n");
    for(int i=1;i<argc;i++)printf("  arg[%d]: %s\n",i,argv[i]);
    return 0;
}
