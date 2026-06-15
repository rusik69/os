/* id.c — print user identity */
#include "unistd.h"
#include "stdio.h"
int main(void){
    printf("uid=%d(root) gid=%d(root) groups=%d(root)\n",getuid(),getgid(),getgid());
    return 0;
}
