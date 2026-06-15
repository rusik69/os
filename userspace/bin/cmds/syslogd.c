/* syslogd.c — system logging daemon */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int foreground=0;
    for(int i=1;i<argc;i++){if(strcmp(argv[i],"-n")==0)foreground=1;}
    printf("syslogd: starting up (pid=%d)\n",getpid());
    printf("syslogd: logging to /var/log/messages (read-only stub)\n");
    printf("syslogd: reading kernel log via /proc/kmsg...\n");
    int fd=open("/proc/kmsg",O_RDONLY,0);
    if(fd>=0){
        unsigned char buf[4096];
        printf("syslogd: /proc/kmsg opened, forwarding to /var/log/messages\n");
        if(!foreground)close(fd);else{
            int n;while((n=read(fd,buf,sizeof(buf)-1))>0){
                buf[n]=0;int ofd=open("/var/log/messages",O_WRONLY|O_CREAT,0644);
                if(ofd>=0){write(ofd,buf,n);close(ofd);}
            }
        }
    }else printf("syslogd: /proc/kmsg not available (kernel syslog service)\n");
    return 0;
}
