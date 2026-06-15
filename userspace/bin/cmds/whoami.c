/* whoami.c — print effective user name */
#include "unistd.h"
#include "stdio.h"
int main(void){
    int uid=geteuid();
    if(uid==0)printf("root\n");
    else printf("user%d\n",uid);
    return 0;
}
