/* trap.c — shell signal handler */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: trap <action> <signal>...\n");printf("trap: use shell built-in for actual signal handling\n");return 1;}
    printf("trap: %s",argv[1]);
    for(int i=2;i<argc;i++)printf(" %s",argv[i]);
    printf("\n");
    return 0;
}
