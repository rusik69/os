/* devmem.c — read/write physical memory */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: devmem <address> [width]\n");return 1;}
    unsigned long addr=0;const char*h=argv[1];if(h[0]=='0'&&(h[1]=='x'||h[1]=='X'))h+=2;
    while(*h){addr=(addr<<4)+(*h>='a'?*h-'a'+10:*h>='A'?*h-'A'+10:*h-'0');h++;}
    int width=argc>2?atoi(argv[2]):32;
    printf("devmem: read 0x%lx (%d bits) = ",addr,width);
    printf("0x%lx (simulated)\n",addr);
    return 0;
}
