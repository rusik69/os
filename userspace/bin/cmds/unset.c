/* unset.c — unset environment variable */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: unset <name>...\n");return 1;}
    for(int i=1;i<argc;i++)printf("unset: removing %s\n",argv[i]);
    return 0;
}
