/* shutdown.c — reboot/shutdown system */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    int reboot_mode = 1; /* default: reboot */

    if(argc>1){
        if(strcmp(argv[1],"-h")==0){
            reboot_mode = 0; /* halt */
        } else if(strcmp(argv[1],"-r")==0){
            reboot_mode = 1; /* reboot */
        } else if(strcmp(argv[1],"-P")==0){
            reboot_mode = 0; /* poweroff = halt */
        }
    }

    printf("Syncing filesystems...\n");
    sync();
    printf("Syncing filesystems again...\n");
    sync();

    if(reboot_mode){
        printf("Rebooting...\n");
    } else {
        printf("Shutting down...\n");
    }

    reboot();
    /* Should not return */
    printf("shutdown: reboot failed\n");
    return 1;
}
