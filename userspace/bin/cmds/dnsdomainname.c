/* dnsdomainname.c — show DNS domain name */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) < 0) {
        printf("dnsdomainname: error\n");
        return 1;
    }
    hostname[sizeof(hostname)-1] = '\0';
    /* Print the domain part after the first dot, if any */
    char *dot = strchr(hostname, '.');
    if (dot && dot[1]) {
        printf("%s\n", dot + 1);
    } else {
        /* No domain, print nothing */
        write(1, "\n", 1);
    }
    return 0;
}
