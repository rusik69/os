/* schedutils.c — get/set scheduling policy */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: schedutils <pid> [policy]\n");return 1;}
    printf("schedutils: pid=%s default=other\n",argv[1]);
    return 0;
}
