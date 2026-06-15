/* shutdown.c — reboot/shutdown */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    printf("Shutting down...\n");
    reboot();
    printf("shutdown: failed\n");
    return 1;
}
