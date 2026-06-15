/* basename.c — strip directory and suffix */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2)return 1;
    char*cp=strrchr(argv[1],'/');
    char*base=cp?cp+1:argv[1];
    if(argc>2){unsigned long slen=strlen(base),slen2=strlen(argv[2]);
        if(slen>slen2&&strcmp(base+slen-slen2,argv[2])==0)base[slen-slen2]=0;}
    write(1,base,strlen(base));write(1,"\n",1);
    return 0;
}
