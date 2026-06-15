/* nft.c — packet filtering and classification */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    (void)argc;(void)argv;
    printf("nft: netfilter configuration\n");
    printf("Available tables: filter, nat, mangle\n");
    printf("Use kernel shell 'nft' command for actual rule management.\n");
    return 0;
}
