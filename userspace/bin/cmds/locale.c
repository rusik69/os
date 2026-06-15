/* locale.c — show locale */
#include "unistd.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    const char*l="LANG=POSIX\nLC_CTYPE=POSIX\nLC_NUMERIC=POSIX\nLC_TIME=POSIX\nLC_COLLATE=POSIX\nLC_MONETARY=POSIX\nLC_MESSAGES=POSIX\nLC_ALL=POSIX\n";
    write(1,l,strlen(l));
    return 0;
}
