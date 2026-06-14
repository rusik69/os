/* pwd.c — Print working directory */
#include "unistd.h"
#include "stdio.h"

int main(void) {
    char buf[512];
    if (getcwd(buf, sizeof(buf)) < 0) {
        printf("pwd: error getting current directory\n");
        return 1;
    }
    printf("%s\n", buf);
    return 0;
}
