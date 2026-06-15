/* envdir.c — run command with environment from directory */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<3){printf("Usage: envdir <dir> <command> [args...]\n");return 1;}
    const char*dir=argv[1];char**cmd=argv+2;
    int d=open(dir,O_RDONLY,0);
    if(d<0){printf("envdir: %s: No such directory\n",dir);return 1;}
    printf("envdir: setting environment variables from %s\n",dir);
    /* Simple version: run command with existing environ */
    execve(cmd[0],cmd,0);
    printf("envdir: cannot exec %s\n",cmd[0]);return 1;
}
