/* printenv.c — print environment variables */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    const char*var=argc>1?argv[1]:0;
    printf("PATH=/bin:/usr/bin\nHOME=/root\nTERM=linux\nUSER=root\nSHELL=/bin/sh\n");
    if(var&&var[0])printf("%s=(set)\n",var);
    return 0;
}
