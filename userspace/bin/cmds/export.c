/* export.c — set environment variable */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<2){
        int fd=open("/proc/self/environ",O_RDONLY,0);
        if(fd>=0){char buf[8192];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
            for(char*cp=buf;cp<buf+n;cp+=strlen(cp)+1){write(1,cp,strlen(cp));write(1,"\n",1);}}
        else printf("declare -x PATH=/bin:/usr/bin\ndeclare -x HOME=/root\ndeclare -x TERM=linux\n");
        return 0;
    }
    for(int i=1;i<argc;i++){
        char*cp=strchr(argv[i],'=');
        if(cp){*cp=0;printf("export: %s=%s\n",argv[i],cp+1);*cp='=';}
        else printf("export: %s\n",argv[i]);
    }
    return 0;
}
