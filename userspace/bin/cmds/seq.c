#include "unistd.h"
#include "stdio.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) { printf("Usage: seq <last> or seq <first> <last>\n"); return 1; }
    int start = 1, end;
    if (argc == 2) end = atoi(argv[1]);
    else { start = atoi(argv[1]); end = atoi(argv[2]); }
    char line[32];
    for (int i = start; i <= end; i++) {
        int len = snprintf(line, sizeof(line), "%d\n", i);
        write(1, line, len);
    }
    return 0;
}
