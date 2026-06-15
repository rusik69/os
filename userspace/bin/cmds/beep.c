/* beep.c — PC speaker beep */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    write(1,"\x07",1);
    if(argc>1)printf("beep: freq=%s\n",argv[1]);
    return 0;
}
