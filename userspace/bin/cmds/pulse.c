/* pulse.c — sound/pulse utility */
#include "unistd.h"
#include "stdio.h"

int main(void){
    printf("Audio subsystem:\n");
    int fd=open("/proc/asound/cards",O_RDONLY,0);
    if(fd>=0){char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;write(1,buf,n);}
    else{printf("  No ALSA /proc interface\n");printf("  Audio: AC97 driver via kernel shell 'play' command\n");}
    return 0;
}
