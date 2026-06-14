/* test.c — simple [ -f, -d, -n, -z, =, != ] tests */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

static int test_file(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return 1;
}

static int test_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return (st.st_mode & 0170000) == 0040000;
}

static int test_exists(const char *path) {
    struct stat st;
    return stat(path, &st) >= 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) return 1;
    int result = 0;
    if (argv[1][0] == '!') {
        /* Handle '!' negation: test ! expr */
        int sub = 0;
        if (argv[2] && argv[2][0] == '-') {
            if (argv[2][1] == 'f') sub = test_file(argv[3]);
            else if (argv[2][1] == 'd') sub = test_dir(argv[3]);
            else if (argv[2][1] == 'e') sub = test_exists(argv[3]);
            else if (argv[2][1] == 'n') sub = (argv[3] && strlen(argv[3]) > 0);
            else if (argv[2][1] == 'z') sub = (argv[3] && strlen(argv[3]) == 0);
        } else if (argv[2] && argv[3] && argv[4]) {
            if (strcmp(argv[3], "=") == 0) sub = (strcmp(argv[2], argv[4]) == 0);
            else if (strcmp(argv[3], "!=") == 0) sub = (strcmp(argv[2], argv[4]) != 0);
        } else if (argv[2]) {
            sub = (strlen(argv[2]) > 0);
        }
        result = !sub;
    } else if (argv[1][0] == '-') {
        if (argv[1][1] == 'f') result = test_file(argv[2]);
        else if (argv[1][1] == 'd') result = test_dir(argv[2]);
        else if (argv[1][1] == 'e') result = test_exists(argv[2]);
        else if (argv[1][1] == 'n') result = (argv[2] && strlen(argv[2]) > 0);
        else if (argv[1][1] == 'z') result = (argv[2] && strlen(argv[2]) == 0);
    } else if (argc >= 4) {
        if (strcmp(argv[2], "=") == 0) result = (strcmp(argv[1], argv[3]) == 0);
        else if (strcmp(argv[2], "!=") == 0) result = (strcmp(argv[1], argv[3]) != 0);
    } else {
        result = (strlen(argv[1]) > 0);
    }
    return result ? 0 : 1;
}
