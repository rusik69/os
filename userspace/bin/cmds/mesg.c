/* mesg.c — control write access to terminal */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc>1&&strcmp(argv[1],"y")==0){printf("mesg: write access enabled\n");return 0;}
    if(argc>1&&strcmp(argv[1],"n")==0){printf("mesg: write access disabled\n");return 0;}
    printf("is y\n");
    return 0;
}
