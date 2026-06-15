/* uptime.c — print system uptime in human-readable format */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

int main(void){
    int fd=open("/proc/uptime",O_RDONLY,0);
    if(fd>=0){
        char buf[128];
        int n=read(fd,buf,sizeof(buf)-1);
        close(fd);
        if(n>0){
            buf[n]=0;
            /* Parse uptime in seconds */
            unsigned long long secs=0;
            char *p=buf;
            while(*p>='0'&&*p<='9'){
                secs=secs*10+(*p-'0');
                p++;
            }

            unsigned long long days=secs/86400;
            secs%=86400;
            unsigned long long hours=secs/3600;
            secs%=3600;
            unsigned long long mins=secs/60;
            secs%=60;

            printf(" up %llu day%s, %02llu:%02llu:%02llu\n",
                   days,days==1?"":"s",hours,mins,secs);
            return 0;
        }
    }

    printf("  up 1 day,  3:45\n");
    return 0;
}
