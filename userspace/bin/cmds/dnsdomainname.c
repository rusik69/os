/* dnsdomainname.c — print DNS domain name */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(void){
    char name[65];
    if(gethostname(name,sizeof(name))>=0){
        char*dot=strchr(name,'.');
        if(dot){write(1,dot+1,strlen(dot+1));write(1,"\n",1);return 0;}
    }
    /* Fallback: read /etc/hostname */
    int fd=open("/etc/hostname",O_RDONLY,0);
    if(fd>=0){char buf[128];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;write(1,buf,n>0?n:0);return 0;}
    printf("localdomain\n");
    return 0;
}
