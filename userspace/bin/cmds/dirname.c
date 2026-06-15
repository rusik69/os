/* dirname.c — strip last component from path */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2)return 1;
    char*cp=strrchr(argv[1],'/');
    if(!cp||cp==argv[1]){write(1,".\n",2);return 0;}
    *cp=0;write(1,argv[1],strlen(argv[1]));write(1,"\n",1);*cp='/';
    return 0;
}
