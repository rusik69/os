/* fg.c — bring job to foreground */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
int main(int argc,char*argv[]){
    int job_id=-1;
    if(argc>1&&argv[1][0]=='%')job_id=atoi(argv[1]+1);
    if(job_id>0)printf("fg: bringing job %d to foreground\n",job_id);
    else printf("fg: use shell built-in for job control\n");
    return 0;
}
