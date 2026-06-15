/* lvcreate.c — create LVM logical volume */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argv;
    if(argc<2){printf("Usage: lvcreate -L <size> -n <name> <vg>\n");return 1;}
    printf("lvcreate: LVM via kernel device mapper\n");
    return 0;
}
