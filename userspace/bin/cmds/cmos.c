/* cmos.c — display CMOS/RTC configuration */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(void){
    printf("CMOS/RTC Configuration:\n");
    int fd=open("/sys/class/rtc/rtc0/date",O_RDONLY,0);
    if(fd>=0){char buf[128];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;printf("  Date: %s",buf);}
    fd=open("/sys/class/rtc/rtc0/time",O_RDONLY,0);
    if(fd>=0){char buf[128];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n>=0?n:0]=0;printf("  Time: %s",buf);}
    printf("  RTC device: /dev/rtc0\n");
    return 0;
}
