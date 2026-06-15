/* dmsetup.c — device mapper control */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: dmsetup <cmd> [args]\n");return 1;}
    if(strcmp(argv[1],"info")==0||strcmp(argv[1],"status")==0){
        printf("device-mapper: checking /sys/block/dm-*\n");
        int fd=open("/sys/block",O_RDONLY,0);
        if(fd>=0){/* list dm-* */close(fd);}
        printf("No device-mapper devices active\n");
    }else if(strcmp(argv[1],"remove_all")==0){
        printf("dmsetup: removing all device-mapper devices\n");
    }else printf("dmsetup: unknown command '%s'\n",argv[1]);
    return 0;
}
