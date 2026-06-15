/* fm.c — file manager */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
int main(int argc,char*argv[]){
    const char*dir=argc>1?argv[1]:".";
    printf("File manager: %s\n",dir);
    printf("Use kernel shell 'fm' for interactive file manager.\n");
    return 0;
}
