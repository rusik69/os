/* color.c — set ANSI console color */
#include "unistd.h"
#include "string.h"
int main(int argc,char*argv[]){
    const char*seq="\033[0m";
    if(argc>1){
        if(strcmp(argv[1],"red")==0)seq="\033[31m";
        else if(strcmp(argv[1],"green")==0)seq="\033[32m";
        else if(strcmp(argv[1],"yellow")==0)seq="\033[33m";
        else if(strcmp(argv[1],"blue")==0)seq="\033[34m";
        else if(strcmp(argv[1],"magenta")==0)seq="\033[35m";
        else if(strcmp(argv[1],"cyan")==0)seq="\033[36m";
        else if(strcmp(argv[1],"white")==0)seq="\033[37m";
        else if(strcmp(argv[1],"bold")==0)seq="\033[1m";
        else seq="\033[0m";
    }
    write(1,seq,strlen(seq));
    return 0;
}
