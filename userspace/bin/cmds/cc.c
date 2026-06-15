/* cc.c — invoke kernel C compiler */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argv;
    if(argc<2){printf("Usage: cc [options] <file>\n");return 1;}
    printf("cc: kernel C compiler (kernel shell 'cc' command)\n");
    return 0;
}
