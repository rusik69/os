/* bg.c — put job in background */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int job_id=-1;
    if(argc>1&&argv[1][0]=='%')job_id=atoi(argv[1]+1);
    if(job_id>0)printf("bg: putting job %d in background\n",job_id);
    else printf("bg: use shell built-in for job control\n");
    return 0;
}
