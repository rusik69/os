/* mkdir.c — create directories */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    int mode=0777,p=0;
    int i=1;
    if(argc>1&&strcmp(argv[1],"-p")==0){p=1;i=2;}
    if(i>=argc){printf("Usage: mkdir [-p] <dir>...\n");return 1;}
    for(;i<argc;i++){
        if(mkdir(argv[i],mode)<0){
            if(!p){printf("mkdir: cannot create '%s'\n",argv[i]);return 1;}
        }
    }
    return 0;
}
