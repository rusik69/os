/* host.c — DNS lookup utility */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: host <hostname>\n");return 1;}
    int result=net_dns(argv[1]);
    if(result>=0){printf("%s has address %d.%d.%d.%d\n",argv[1],
        (result>>24)&0xFF,(result>>16)&0xFF,(result>>8)&0xFF,result&0xFF);}
    else {printf("%s has address (DNS failed)\n",argv[1]);return 1;}
    return 0;
}
