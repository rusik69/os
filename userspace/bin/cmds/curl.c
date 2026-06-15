/* curl.c — HTTP client */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    const char*url=0;
    for(int i=1;i<argc;i++){if(argv[i][0]!='-')url=argv[i];}
    if(!url){printf("Usage: curl <url>\n");return 1;}
    printf("curl: %s\n",url);
    /* Parse URL */
    if(strncmp(url,"http://",7)==0)printf("  Protocol: HTTP\n  Host: %s\n",url+7);
    else printf("  Only HTTP URLs supported\n");
    return 0;
}
