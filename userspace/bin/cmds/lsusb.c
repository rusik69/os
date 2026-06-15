/* lsusb.c — list USB devices stub */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("USB device info can be obtained from the kernel shell "
           "'lsusb' command or by checking /sys/bus/usb/devices/.\n");
    return 0;
}
