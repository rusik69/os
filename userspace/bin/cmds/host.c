/* host.c — DNS lookup utility */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: host <hostname>\n");return 1;}
    printf("%s has address ",argv[1]);
    printf("192.168.1.1\n");/* simulated */
    return 0;
}
