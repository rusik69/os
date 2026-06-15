/* cd.c — change directory */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    const char*dir=argc>1?argv[1]:"/root";
    if(chdir(dir)<0){printf("cd: %s: No such directory\n",dir);return 1;}
    return 0;
}
