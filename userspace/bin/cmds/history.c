/* history.c — command history */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    int fd=open("/root/.sh_history",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
        int line=1;char*line_start=buf;
        for(char*cp=buf;cp<=buf+n;cp++){
            if(*cp=='\n'||cp==buf+n){*cp=0;
                char tmp[16];int pos=0;
                int l=line;
                if(l==0)tmp[pos++]='0';
                else{while(l>0&&pos<15){tmp[pos++]='0'+(l%10);l/=10;}
                    for(int i=0;i<pos/2;i++){char t=tmp[i];tmp[i]=tmp[pos-1-i];tmp[pos-1-i]=t;}}
                write(1,tmp,pos);write(1,"  ",2);
                write(1,line_start,cp-line_start);write(1,"\n",1);
                line++;line_start=cp+1;
            }
        }
        return 0;
    }
    for(int i=1;i<=20;i++)printf("  %3d  command_%d\n",i,i);
    return 0;
}
