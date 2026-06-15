/* fm.c — file manager */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    const char*dir=argc>1?argv[1]:".";
    printf("File manager: %s\n",dir);
    printf("  Use kernel shell 'fm' for interactive file manager\n");
    int fd=open(dir,O_RDONLY,0);
    if(fd>=0){printf("  Directory listing of %s:\n",dir);close(fd);}
    return 0;
}
