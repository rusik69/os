/* 404.c — command not found display */
#include "unistd.h"
#include "string.h"

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    const char *msg = "404: Not Found\n";
    write(1, msg, strlen(msg));
    return 1;
}
