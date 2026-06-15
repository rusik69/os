/* pinky.c — lightweight finger */
#include "unistd.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc>1)printf("Login: %s\n",argv[1]);
    printf("Login: root     Name: Superuser\n");
    printf("Directory: /root                Shell: /bin/sh\n");
    printf("On since Mon 17:08 on ttyS0\n");
    return 0;
}
