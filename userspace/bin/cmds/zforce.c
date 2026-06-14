#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: zforce FILE...\n");
        return 1;
    }
    /* Add .gz extension to files missing it */
    for (int i = 1; i < argc; i++) {
        int len = strlen(argv[i]);
        if (len < 4 || argv[i][len-1] != 'z' || argv[i][len-2] != 'g' || argv[i][len-3] != '.') {
            /* Would need rename to add .gz */
            printf("zforce: %s: not implemented\n", argv[i]);
        }
    }
    return 0;
}
