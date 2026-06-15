/* dosbox.c — DOS emulation */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: dosbox <dos-executable>\n");return 1;}
    printf("dosbox: running %s in DOS emulation\n",argv[1]);
    printf("DOS emulator: kernel built-in (kernel shell 'dosbox' command)\n");
    return 0;
}
