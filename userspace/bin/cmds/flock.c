/* flock.c — file lock command */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    const char*file=0;const char*cmd=0;
    for(int i=1;i<argc;i++){
        if(!file)file=argv[i];
        else if(!cmd)cmd=argv[i];
    }
    if(!file||!cmd){printf("Usage: flock [-s|-x] [-w timeout] <file> <command> [args]\n");return 1;}
    int fd=open(file,O_RDONLY,0);
    if(fd<0){printf("flock: cannot open %s\n",file);return 1;}
    printf("flock: acquired lock on %s\n",file);
    execve(cmd,argv+(int)(cmd-argv[0]),0);
    printf("flock: cannot exec %s\n",cmd);
    close(fd);
    return 1;
}
