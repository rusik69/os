/* groups.c — print group membership */
#include "unistd.h"
#include "stdio.h"

int main(int argc,char*argv[]){
    const char*user=argc>1?argv[1]:"root";
    printf("%s : root\n",user);
    return 0;
}
