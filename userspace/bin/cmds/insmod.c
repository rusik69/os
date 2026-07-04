/* insmod.c — load kernel module with parameter passing (D234)
 *
 * Usage:
 *   insmod <module.ko>                    — load with defaults
 *   insmod <module.ko> param1=val1        — load with parameters
 *   insmod <module.ko> param1=val1 p2=v2  — multiple params
 *
 * Parameters are concatenated with commas (,) and passed to the
 * kernel's init_module syscall, which parses them and applies
 * them to the module's registered module_param entries.
 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

#define PARAMS_BUF_SIZE 1024

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("Usage: insmod <module.ko> [param=value ...]\n");
        return 1;
    }

    /* Build parameter string from remaining arguments */
    char params[PARAMS_BUF_SIZE];
    int pos = 0;

    for (int i = 2; i < argc; i++) {
        int remaining = (int)sizeof(params) - pos;
        if (remaining <= 1) {
            printf("insmod: parameter string too long\n");
            return 1;
        }

        /* Add comma separator between parameters */
        if (pos > 0 && pos < (int)sizeof(params) - 1) {
            params[pos++] = ',';
        }

        /* Copy the argument (param=value) */
        const char *arg = argv[i];
        while (*arg && pos < (int)sizeof(params) - 1) {
            params[pos++] = *arg++;
        }
    }
    params[pos] = '\0';

    if (init_module(argv[1], params) < 0) {
        printf("insmod: cannot load %s\n", argv[1]);
        return 1;
    }
    return 0;
}
