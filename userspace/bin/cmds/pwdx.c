/* pwdx.c — print working directory of process */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: pwdx <pid>...\n");return 1;}
    for(int i=1;i<argc;i++){
        int pid=atoi(argv[i]);
        char path[64];snprintf(path,sizeof(path),"/proc/%d/cwd",pid);
        char link[512];int n=readlink(path,link,sizeof(link)-1);
        if(n>0){link[n]=0;printf("%d: %s\n",pid,link);}
        else printf("%d: No such process\n",pid);
    }
    return 0;
}
