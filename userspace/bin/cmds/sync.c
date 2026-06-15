/* sync.c — synchronize cached writes to persistent storage */
#include "unistd.h"
#include "stdio.h"
int main(void){
    sync();
    return 0;
}
