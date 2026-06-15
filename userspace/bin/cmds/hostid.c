/* hostid.c — print host identifier (based on hostname hash) */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(void){
    char name[65]; unsigned long id=0;
    if(gethostname(name,sizeof(name))>=0){
        for(int i=0;name[i];i++)id=id*31+name[i];
        printf("%08lx\n",id);
    } else printf("007f0100\n");
    return 0;
}
