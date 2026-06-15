/* printenv.c — print environment variables */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    int fd=open("/proc/self/environ",O_RDONLY,0);
    if(fd<0){printf("PATH=/bin:/usr/bin\nHOME=/root\nTERM=linux\n");return 0;}
    char buf[8192];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
    /* environ is null-separated */
    const char*var=argc>1?argv[1]:0;
    if(var){
        char*cp=buf;int vlen=strlen(var);
        while(cp<buf+n){
            if(strncmp(cp,var,vlen)==0&&cp[vlen]=='='){
                write(1,cp+vlen+1,strlen(cp+vlen));write(1,"\n",1);return 0;
            }
            cp+=strlen(cp)+1;
        }
        return 1;
    }
    for(char*cp=buf;cp<buf+n;cp+=strlen(cp)+1){write(1,cp,strlen(cp));write(1,"\n",1);}
    return 0;
}
