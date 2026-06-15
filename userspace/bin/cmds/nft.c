/* nft.c — packet filtering */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    printf("nft: netfilter configuration\n");
    printf("Use kernel shell 'nft' for rule management.\n");
    return 0;
}
