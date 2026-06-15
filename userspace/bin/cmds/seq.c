/* seq.c — print sequence of numbers */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    int start=1,end=1,step=1;
    if(argc==2)end=atoi(argv[1]);
    else if(argc==3){start=atoi(argv[1]);end=atoi(argv[2]);}
    else if(argc==4){start=atoi(argv[1]);step=atoi(argv[2]);end=atoi(argv[3]);}
    else {printf("Usage: seq [start] <end>\n");return 1;}
    if(step==0)return 1;
    if(step>0){for(int i=start;i<=end;i+=step)printf("%d\n",i);}
    else {for(int i=start;i>=end;i+=step)printf("%d\n",i);}
    return 0;
}
