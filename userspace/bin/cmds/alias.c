/* alias.c — define command aliases */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("alias: current aliases:\n");printf("  ll=ls -l\n  la=ls -a\n");return 0;}
    for(int i=1;i<argc;i++){char*cp=strchr(argv[i],'=');
        if(cp){*cp=0;printf("alias: %s='%s'\n",argv[i],cp+1);*cp='=';}
        else printf("alias: %s=''\n",argv[i]);}
    return 0;
}
