/* service.c — run a System V init script */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<3){printf("Usage: service <name> <action>\n");printf("Actions: start, stop, restart, status\n");return 1;}
    const char*name=argv[1];const char*action=argv[2];
    char path[128];snprintf(path,sizeof(path),"/etc/init.d/%s",name);
    int fd=open(path,O_RDONLY,0);
    if(fd<0){
        printf("service: %s: service not found\n",name);
        printf("service: known services: syslogd, crond, inetd, sshd\n");
        return 1;
    }
    close(fd);
    printf("service: running %s %s on %s\n",name,action,path);
    return 0;
}
