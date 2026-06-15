/* cd.c — change directory and print working directory */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    const char*dir=argc>1?argv[1]:"/root";
    if(chdir(dir)<0){
        printf("cd: %s: No such directory\n",dir);
        return 1;
    }
    /* Print working directory after change */
    char cwd[256];
    if(getcwd(cwd,sizeof(cwd))>=0){
        write(1,cwd,strlen(cwd));
        write(1,"\n",1);
    }
    return 0;
}
