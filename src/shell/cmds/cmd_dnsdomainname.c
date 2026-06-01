/* cmd_dnsdomainname.c — print DNS domain name */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"

int cmd_dnsdomainname(int argc, char **argv) {
    (void)argc;
    (void)argv;
    /* Get hostname and derive domain name */
    struct utsname uts;
    memset(&uts, 0, sizeof(uts));

    /* Try reading /etc/hostname or just return a default */
    char buf[128];
    uint32_t out_size;

    if (libc_fs_read_file("/etc/hostname", buf, sizeof(buf) - 1, &out_size) == 0) {
        buf[out_size] = '\0';
        /* Remove trailing newline */
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r'))
            buf[--len] = '\0';
        /* Look for domain part after first dot */
        char *dot = strchr(buf, '.');
        if (dot) {
            kprintf("%s\n", dot + 1);
        } else {
            kprintf("(none)\n");
        }
    } else {
        kprintf("(none)\n");
    }
    return 0;
}
