/* unlink.c — remove file (unlink) */
#include "unistd.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: unlink <file>\n");return 1;}
    if(unlink(argv[1])<0){printf("unlink: failed\n");return 1;}
    return 0;
}
