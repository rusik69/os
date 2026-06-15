/* insmod.c — insert kernel module from .ko file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: insmod MODULE.ko [PARAMS]\n");
        return 1;
    }

    const char *path = argv[1];
    const char *params = (argc > 2) ? argv[2] : NULL;

    int ret = init_module(path, params);
    if (ret < 0) {
        printf("insmod: failed to insert '%s'", path);
        if (params) printf(" (params: %s)", params);
        printf("\n");
        return 1;
    }

    printf("insmod: inserted '%s' (module_id=%d)\n", path, ret);
    return 0;
}
