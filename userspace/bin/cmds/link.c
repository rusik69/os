/* link.c — create a hard link */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    if(argc<3){printf("Usage: link <target> <link_name>\n");return 1;}
    if(link(argv[1],argv[2])<0){printf("link: failed\n");return 1;}
    return 0;
}
