/* hostname.c — print/set hostname */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    (void)argv;
    if(argc>1){printf("hostname: set via kernel shell\n");return 1;}
    char name[65];
    if(gethostname(name,sizeof(name))<0){printf("hostname\n");return 1;}
    write(1,name,strlen(name));write(1,"\n",1);
    return 0;
}
