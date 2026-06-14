/* ethtool.c — Ethernet device settings (stub) */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("usage: ethtool <interface>\n");
        return 1;
    }
    (void)argv;
    printf("ethtool: not supported\n");
    return 1;
}
