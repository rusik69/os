/* uname.c — system info via uname() syscall */

#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    /* Parse flags */
    int show_all = 1;
    int show_sysname = 0;
    int show_nodename = 0;
    int show_release = 0;
    int show_version = 0;
    int show_machine = 0;

    if (argc > 1) {
        show_all = 0;
        for (int i = 1; i < argc; i++) {
            char *a = argv[i];
            if (a[0] == '-') {
                while (*++a) {
                    if (*a == 'a') show_all = 1;
                    else if (*a == 's') show_sysname = 1;
                    else if (*a == 'n') show_nodename = 1;
                    else if (*a == 'r') show_release = 1;
                    else if (*a == 'v') show_version = 1;
                    else if (*a == 'm') show_machine = 1;
                }
            }
        }
        if (!show_all && !show_sysname && !show_nodename &&
            !show_release && !show_version && !show_machine)
            show_sysname = 1;
    }

    struct utsname uts;
    if (uname(&uts) < 0) {
        printf("uname: failed\n");
        return 1;
    }

    if (show_all || show_sysname)  { printf("%s ", uts.sysname); }
    if (show_all || show_nodename) { printf("%s ", uts.nodename); }
    if (show_all || show_release)  { printf("%s ", uts.release); }
    if (show_all || show_version)  { printf("%s ", uts.version); }
    if (show_all || show_machine)  { printf("%s ", uts.machine); }
    printf("\n");

    return 0;
}
