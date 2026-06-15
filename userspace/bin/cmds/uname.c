/* uname.c — print system information */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    struct utsname buf;
    if(uname(&buf)<0){printf("unknown\n");return 1;}
    if(argc>1&&strcmp(argv[1],"-a")==0){
        printf("%s %s %s %s %s\n",buf.sysname,buf.nodename,buf.release,buf.version,buf.machine);
    } else if(argc>1&&strcmp(argv[1],"-s")==0){printf("%s\n",buf.sysname);
    } else if(argc>1&&strcmp(argv[1],"-n")==0){printf("%s\n",buf.nodename);
    } else if(argc>1&&strcmp(argv[1],"-r")==0){printf("%s\n",buf.release);
    } else if(argc>1&&strcmp(argv[1],"-m")==0){printf("%s\n",buf.machine);
    } else {printf("%s\n",buf.sysname);}
    return 0;
}
