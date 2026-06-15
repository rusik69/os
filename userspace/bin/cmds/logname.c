/* logname.c — print login name */
#include "unistd.h"
#include "stdio.h"
int main(void){
    int uid=getuid();
    if(uid==0)printf("root\n");
    else printf("user%d\n",uid);
    return 0;
}
