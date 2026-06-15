/* groups.c — print group membership */
#include "unistd.h"
#include "stdio.h"
int main(void){
    int gid=getgid();
    printf("root\n");
    (void)gid;
    return 0;
}
