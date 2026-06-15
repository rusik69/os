/* sleep.c — delay for specified amount of time */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: sleep <seconds>\n");return 1;}
    int secs=atoi(argv[1]);
    if(secs<=0)return 0;
    struct timespec ts;
    ts.tv_sec=secs;ts.tv_nsec=0;
    nanosleep(&ts,0);
    return 0;
}
