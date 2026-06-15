/* setarch.c — print/set architecture */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc>1){
        if(strcmp(argv[1],"--list")==0){printf("x86_64\nlinux32\n");return 0;}
        printf("setarch: use 'exec' to run with arch\n");return 1;
    }
    struct utsname buf;
    if(uname(&buf)>=0){printf("%s\n",buf.machine);return 0;}
    printf("x86_64\n");
    return 0;
}
