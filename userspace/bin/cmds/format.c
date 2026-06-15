/* format.c — format filesystem */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: format <device> [fstype]\n");return 1;}
    const char*dev=argv[1];const char*fstype=argc>2?argv[2]:"smfs";
    printf("format: formatting %s as %s\n",dev,fstype);
    printf("format: use kernel shell 'format' command.\n");
    return 0;
}
