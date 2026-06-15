/* yes.c — output a string repeatedly */
#include "unistd.h"
#include "string.h"
int main(int argc,char*argv[]){
    const char*s=argc>1?argv[1]:"y";
    unsigned long slen=strlen(s);
    while(1){write(1,s,slen);write(1,"\n",1);}
    return 0;
}
