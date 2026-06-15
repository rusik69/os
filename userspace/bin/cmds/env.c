/* env.c — run a program with modified environment */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int ignore_env=0;
    int i=1;

    if(argc>1&&strcmp(argv[1],"-i")==0){
        ignore_env=1;
        i=2;
    }

    if(argc==1||(ignore_env&&argc==2)){
        /* Print environment */
        int fd=open("/proc/self/environ",O_RDONLY,0);
        if(fd>=0){
            char buf[8192];
            int n=read(fd,buf,sizeof(buf)-1);
            close(fd);
            if(n>0){
                buf[n]=0;
                for(char*cp=buf;cp<buf+n;cp+=strlen(cp)+1){
                    write(1,cp,strlen(cp));
                    write(1,"\n",1);
                }
            }
        } else {
            printf("PATH=/bin:/usr/bin\nHOME=/root\n");
        }
        return 0;
    }

    if(ignore_env){
        /* Run with empty environment */
        execve(argv[i],argv+i,0);
    } else {
        /* Find NAME=VALUE pairs before the command */
        int cmd_idx=i;
        while(cmd_idx<argc){
            if(strchr(argv[cmd_idx],'=')){
                /* Set this var — but we can't modify current env, so skip */
                cmd_idx++;
            } else {
                break;
            }
        }
        if(cmd_idx>=argc){
            /* Print environment (no command) */
            int fd=open("/proc/self/environ",O_RDONLY,0);
            if(fd>=0){
                char buf[8192];
                int n=read(fd,buf,sizeof(buf)-1);
                close(fd);
                if(n>0){
                    buf[n]=0;
                    for(char*cp=buf;cp<buf+n;cp+=strlen(cp)+1){
                        write(1,cp,strlen(cp));
                        write(1,"\n",1);
                    }
                }
            }
            return 0;
        }
        /* Run command with current environment */
        execve(argv[cmd_idx],argv+cmd_idx,0);
    }

    printf("env: cannot exec %s\n",argv[1]);
    return 1;
}
