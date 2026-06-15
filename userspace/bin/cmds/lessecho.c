/* lessecho.c — echo arguments with metacharacter escaping */
#include "unistd.h"
#include "string.h"

int main(int argc,char*argv[]){
    for(int i=1;i<argc;i++){
        unsigned long len=strlen(argv[i]);
        for(unsigned long j=0;j<len;j++){
            char c=argv[i][j];
            if(c==' '||c=='\t'||c=='\\'||c=='\"'||c=='\'')
                write(1,"\\",1);
            write(1,&c,1);
        }
        if(i<argc-1)write(1," ",1);
    }
    write(1,"\n",1);
    return 0;
}
