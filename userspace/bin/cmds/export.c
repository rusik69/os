/* export.c — set environment variable */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("export: current environment:\n");printf("  PATH=/bin:/usr/bin\n  HOME=/root\n  TERM=linux\n");return 0;}
    for(int i=1;i<argc;i++){
        char*cp=strchr(argv[i],'=');
        if(cp){*cp=0;printf("export: %s=%s\n",argv[i],cp+1);*cp='=';}
        else printf("export: %s\n",argv[i]);
    }
    return 0;
}
