/* echo.c — echo arguments to stdout */
#include "unistd.h"
#include "string.h"
int main(int argc,char*argv[]){
    int no_newline=0,start=1;
    if(argc>1&&strcmp(argv[1],"-n")==0){no_newline=1;start=2;}
    for(int i=start;i<argc;i++){
        if(i>start)write(1," ",1);
        write(1,argv[i],strlen(argv[i]));
    }
    if(!no_newline)write(1,"\n",1);
    return 0;
}
