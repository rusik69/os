/* reset.c — reset terminal */
#include "unistd.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    write(1,"\033c",2);
    write(1,"\033[2J",4);
    write(1,"\033[H",3);
    write(1,"\033[?25h",6);
    return 0;
}
