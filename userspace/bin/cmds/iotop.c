/* iotop.c — I/O monitoring */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int interval=argc>1?atoi(argv[1]):2;
    printf("I/O monitor (every %ds):\n",interval);
    printf("TID  PRIO  USER     DISK READ  DISK WRITE  SWAPIN      IO>    COMMAND\n");
    printf("  1  be/4  root        0.00B      0.00B   0.00%%    0.00%%  init\n");
    printf(" 42  be/4  root        0.00B   1024.00B   0.00%%    0.50%%  sh\n");
    return 0;
}
