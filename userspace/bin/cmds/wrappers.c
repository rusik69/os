/* wrappers.c — wrapper information */
#include "unistd.h"
#include "string.h"
int main(void){
    const char*msg="wrappers: use kernel shell for wrapper functionality\n";
    write(1,msg,strlen(msg));
    return 0;
}
