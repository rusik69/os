/* nproc.c — print number of processing units */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(void){
    int fd=open("/sys/devices/system/cpu/online",O_RDONLY,0);
    if(fd>=0){char buf[64];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;
        /* Parse: "0-3" -> 4, "0-1,4-5" -> 4, "0" -> 1 */
        int total=0;char*cp=buf;int a=0,b=0;
        while((*cp>='0'&&*cp<='9')||*cp=='-'||*cp==','||*cp=='\n'){
            if(*cp>='0'&&*cp<='9'){a=0;while(*cp>='0'&&*cp<='9'){a=a*10+(*cp-'0');cp++;}}
            else if(*cp=='-'){cp++;b=0;while(*cp>='0'&&*cp<='9'){b=b*10+(*cp-'0');cp++;}
                total+=b-a+1;a=b;}
            else if(*cp==','){total+=1;cp++;}else cp++;
        }
        if(total>0)printf("%d\n",total);else printf("1\n");
    } else printf("1\n");
    return 0;
}
