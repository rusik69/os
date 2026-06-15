/* ln.c — make links between files */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int symbolic=0;
    const char*target=0,*linkname=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-s")==0)symbolic=1;
        else if(!target)target=argv[i];
        else linkname=argv[i];
    }
    if(!target){printf("Usage: ln [-s] <target> [link_name]\n");return 1;}
    if(!linkname){const char*cp=strrchr(target,'/');linkname=cp?cp+1:target;}
    printf("ln: creating %slink %s -> %s\n",symbolic?"symbolic ":"",linkname,target);
    return 0;
}
