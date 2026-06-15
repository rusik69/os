/* fgconsole.c — print foreground console number */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(void){
    int fd=open("/sys/class/tty/console/active",O_RDONLY,0);
    if(fd>=0){char buf[16];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
        char*cp=buf;while(*cp&&*cp<'0')cp++;/* skip to first digit */
        if(*cp){write(1,cp,1);write(1,"\n",1);return 0;}}
    write(1,"0\n",2);
    return 0;
}
