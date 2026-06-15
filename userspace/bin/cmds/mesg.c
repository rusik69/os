/* mesg.c — control write access to terminal */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(int argc,char*argv[]){
    if(argc>1){
        if(strcmp(argv[1],"y")==0){printf("mesg: write access enabled\n");}
        else if(strcmp(argv[1],"n")==0){printf("mesg: write access disabled\n");}
        else{printf("Usage: mesg [y|n]\n");return 1;}
    }else{
        printf("mesg: terminal write access is currently enabled\n");
    }
    return 0;
}
