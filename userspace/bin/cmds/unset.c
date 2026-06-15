/* unset.c — unset environment variable */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: unset <name>...\n");
        printf("Note: environment changes affect only subprocesses via execve.\n");
        return 1;
    }
    for(int i=1;i<argc;i++){
        /* We can't actually unset from the kernel environ without setenv() libc support.
           Instead, inform the user and show how it works. */
        printf("unset: %s would be removed from environment\n",argv[i]);
    }
    return 0;
}
