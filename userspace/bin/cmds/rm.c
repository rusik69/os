/* rm.c — remove files or directories */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    int rflag=0;
    int i=1;
    if(argc>1&&strcmp(argv[1],"-r")==0){rflag=1;i=2;}
    if(i>=argc){printf("Usage: rm [-r] <file>...\n");return 1;}
    for(;i<argc;i++){
        if(rflag)rmdir(argv[i]);
        else unlink(argv[i]);
    }
    return 0;
}
