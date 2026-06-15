/* modinfo.c — module information */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: modinfo <module>\n");return 1;}
    char buf[4096];
    if(query_module(argv[1],buf,sizeof(buf))<0){
        printf("modinfo: module '%s' not found\n",argv[1]);return 1;}
    write(1,buf,strlen(buf));write(1,"\n",1);
    return 0;
}
