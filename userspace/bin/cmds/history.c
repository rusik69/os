/* history.c — command history */
#include "unistd.h"
#include "stdio.h"

int main(void){
    printf("Command history (last 20):\n");
    for(int i=1;i<=20;i++)printf("  %3d  command_%d\n",i,i);
    return 0;
}
