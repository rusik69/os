/* systemctl.c — System control */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: systemctl <start|stop|restart|status> <service>\n");
        return 1;
    }
    const char *cmd = argv[1];
    const char *svc = argc >= 3 ? argv[2] : "";
    if (strcmp(cmd, "start") == 0)
        printf("systemctl: starting %s\n", svc);
    else if (strcmp(cmd, "stop") == 0)
        printf("systemctl: stopping %s\n", svc);
    else if (strcmp(cmd, "restart") == 0)
        printf("systemctl: restarting %s\n", svc);
    else if (strcmp(cmd, "status") == 0){
        char pidpath[256];int p=0;
        const char*pref="/var/run/";while(*pref)pidpath[p++]=*pref++;
        while(*svc)pidpath[p++]=*svc++;
        const char*suf=".pid";while(*suf)pidpath[p++]=*suf++;
        pidpath[p]=0;
        int fd=open(pidpath,O_RDONLY,0);
        if(fd>=0){char pid[16];int n=read(fd,pid,sizeof(pid)-1);close(fd);pid[n>=0?n:0]=0;
            printf("systemctl: %s running (pid %s)\n",argv[2],pid);}
        else printf("systemctl: %s not running\n",argv[2]);
    }
    else if (strcmp(cmd, "list-units") == 0)
        printf("systemctl: no units\n");
    else
        printf("systemctl: unknown command '%s'\n", cmd);
    return 0;
}
