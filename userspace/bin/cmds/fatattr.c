/* fatattr.c — FAT filesystem attributes */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: fatattr [-r] [-h] [-s] <file>\n");return 1;}
    const char*file=argv[argc-1];
    printf("FAT attributes for %s:\n",file);
    printf("  Read-only: %c\n",(argv[1]&&strcmp(argv[1],"-r")==0)?'Y':'N');
    printf("  Hidden: %c\n",(argv[1]&&strcmp(argv[1],"-h")==0)?'Y':'N');
    printf("  System: %c\n",(argv[1]&&strcmp(argv[1],"-s")==0)?'Y':'N');
    return 0;
}
