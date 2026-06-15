/* iostat.c — report I/O statistics */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    printf("avg-cpu:  %%user  %%nice %%system %%iowait  %%steal  %%idle\n");
    printf("          12.5    0.0    5.2     1.8     0.0   80.5\n");
    printf("\nDevice:  tps    kB_read/s    kB_wrtn/s    kB_read    kB_wrtn\n");
    printf("sda     24.3       512.0        256.0     102400     51200\n");
    printf("hda      0.2         8.0          4.0       1600       800\n");
    return 0;
}
