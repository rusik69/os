/* ln.c — create links */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    int sflag=0;
    int i=1;
    if(argc>1&&strcmp(argv[1],"-s")==0){sflag=1;i=2;}
    if(argc-i<2){printf("Usage: ln [-s] <target> <link>\n");return 1;}
    if(sflag){
        if(symlink(argv[i],argv[i+1])<0){printf("ln: symlink failed\n");return 1;}
    } else {
        if(link(argv[i],argv[i+1])<0){printf("ln: link failed\n");return 1;}
    }
    return 0;
}
