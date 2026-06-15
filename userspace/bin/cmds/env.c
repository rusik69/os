/* env.c — run a program with modified environment */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc==1){
        int fd=open("/proc/self/environ",O_RDONLY,0);
        if(fd>=0){char buf[8192];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
            for(char*cp=buf;cp<buf+n;cp+=strlen(cp)+1){write(1,cp,strlen(cp));write(1,"\n",1);}}
        else printf("PATH=/bin:/usr/bin\nHOME=/root\n");
        return 0;
    }
    execve(argv[1],argv+1,0);
    printf("env: cannot exec %s\n",argv[1]);return 1;
}
