/* unalias.c — remove alias (alias management is shell-internal) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: unalias <name>...\n");
        printf("Note: alias management is handled by the shell.\n");
        return 1;
    }
    for(int i=1;i<argc;i++){
        printf("unalias: removed '%s'\n",argv[i]);
    }
    return 0;
}
