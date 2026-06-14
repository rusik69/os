#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc > 1) {
        if (strcmp(argv[1], "y") == 0 || strcmp(argv[1], "yes") == 0) {
            printf("mesg: message display enabled (not implemented)\n");
        } else if (strcmp(argv[1], "n") == 0 || strcmp(argv[1], "no") == 0) {
            printf("mesg: message display disabled (not implemented)\n");
        } else {
            printf("Usage: mesg [y|n]\n");
            return 1;
        }
    } else {
        printf("mesg: is y (not implemented)\n");
    }
    return 0;
}
