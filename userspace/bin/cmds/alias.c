/* alias.c — define/display aliases */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){
        /* Display known aliases from /etc/aliases or file */
        int fd=open("/etc/aliases",O_RDONLY,0);
        if(fd>=0){
            char buf[4096];
            int n=read(fd,buf,sizeof(buf)-1);
            close(fd);
            if(n>0){
                buf[n]=0;
                write(1,buf,n);
                return 0;
            }
        }
        /* Fallback default aliases */
        printf("alias: ll='ls -l'\n");
        printf("alias: la='ls -a'\n");
        return 0;
    }

    /* Parse NAME=VALUE and display */
    for(int i=1;i<argc;i++){
        char*cp=strchr(argv[i],'=');
        if(cp){
            *cp=0;
            printf("alias: %s='%s'\n",argv[i],cp+1);
            *cp='=';
        } else {
            printf("alias: %s=''\n",argv[i]);
        }
    }
    return 0;
}
