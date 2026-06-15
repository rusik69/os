/* pwdx.c — print working directory of process */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: pwdx <pid>...\n");return 1;}
    for(int i=1;i<argc;i++){
        char path[64];int p=0;
        const char*pref="/proc/";while(*pref)path[p++]=*pref++;
        const char*pid=argv[i];while(*pid)path[p++]=*pid++;
        const char*suf="/cwd";while(*suf)path[p++]=*suf++;
        path[p]=0;
        char buf[512];int n=readlink(path,buf,sizeof(buf)-1);
        if(n<0){printf("%s: No such process\n",argv[i]);continue;}
        buf[n]=0;printf("%s: %s\n",argv[i],buf);
    }
    return 0;
}
