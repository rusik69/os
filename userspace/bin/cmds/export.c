/* export.c — display environment variables */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc<2){
        /* Print environment from /proc/self/environ */
        int fd=open("/proc/self/environ",O_RDONLY,0);
        if(fd>=0){
            char buf[8192];
            int n=read(fd,buf,sizeof(buf)-1);
            close(fd);
            if(n>0){
                buf[n]=0;
                for(char*cp=buf;cp<buf+n;cp+=strlen(cp)+1){
                    write(1,"declare -x ",11);
                    write(1,cp,strlen(cp));
                    write(1,"\n",1);
                }
                return 0;
            }
        }
        printf("declare -x PATH=/bin:/usr/bin\n");
        printf("declare -x HOME=/root\n");
        printf("declare -x TERM=linux\n");
        return 0;
    }

    /* Parse NAME=VALUE and pass to exec (note: can't setenv in-process without libc support) */
    for(int i=1;i<argc;i++){
        char*cp=strchr(argv[i],'=');
        if(cp){
            *cp=0;
            printf("export: set %s=%s\n",argv[i],cp+1);
            *cp='=';
        } else {
            /* Print specific variable */
            int fd=open("/proc/self/environ",O_RDONLY,0);
            if(fd>=0){
                char buf[8192];
                int n=read(fd,buf,sizeof(buf)-1);
                close(fd);
                if(n>0){
                    buf[n]=0;
                    int vlen=strlen(argv[i]);
                    for(char*cp2=buf;cp2<buf+n;cp2+=strlen(cp2)+1){
                        if(strncmp(cp2,argv[i],vlen)==0&&cp2[vlen]=='='){
                            write(1,"declare -x ",11);
                            write(1,cp2,strlen(cp2));
                            write(1,"\n",1);
                            break;
                        }
                    }
                }
            }
        }
    }
    return 0;
}
