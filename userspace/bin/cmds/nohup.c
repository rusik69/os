/* nohup.c — run command immune to hangups */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: nohup <command> [args...]\n");
        return 1;
    }

    /* Ignore SIGHUP (signal 1) — SIG_IGN = (void(*)(int))1 */
    signal(1,(void(*)(int))1);

    /* Redirect stdin from /dev/null */
    int fd=open("/dev/null",O_RDONLY,0);
    if(fd>=0){dup2(fd,0);close(fd);}

    /* Redirect stdout to nohup.out */
    int ofd=open("nohup.out",O_WRONLY|O_CREAT|O_APPEND,0644);
    if(ofd>=0){
        dup2(ofd,1);
        close(ofd);
        /* Write message to stderr */
        write(2,"nohup: appending output to 'nohup.out'\n",39);
    }

    /* Execute command */
    execve(argv[1],argv+1,0);
    printf("nohup: cannot exec %s\n",argv[1]);
    return 1;
}
