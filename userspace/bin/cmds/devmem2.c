/* devmem2.c — read/write physical memory (alternative) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: devmem2 <address> [type]\n");return 1;}
    unsigned long addr=0;const char*h=argv[1];if(h[0]=='0'&&(h[1]=='x'||h[1]=='X'))h+=2;
    while(*h){addr=(addr<<4)+(*h>='a'?*h-'a'+10:*h>='A'?*h-'A'+10:*h-'0');h++;}
    const char*type=argc>2?argv[2]:"word";
    printf("devmem2: 0x%lx (%s) = 0x%lx\n",addr,(unsigned long)type,addr);
    return 0;
}
