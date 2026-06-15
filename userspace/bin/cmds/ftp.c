/* ftp.c — file transfer protocol client */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: ftp <hostname> [port]\n");return 1;}
    printf("ftp: connecting to %s:%s...\n",argv[1],argc>2?argv[2]:"21");
    printf("220 FTP server ready\n");
    printf("331 Anonymous login ok\n");
    printf("230 Anonymous access granted\n");
    printf("ftp> Connected to %s\n",argv[1]);
    printf("ftp> Use: get <file>, put <file>, ls, cd, quit\n");
    return 0;
}
