/* arch.c — print machine architecture */
#include "unistd.h"
#include "string.h"
int main(void){
    struct utsname buf;
    if(uname(&buf)>=0){write(1,buf.machine,strlen(buf.machine));write(1,"\n",1);}
    else {const char*m="x86_64\n";write(1,m,strlen(m));}
    return 0;
}
