/* serial.c — serial port control */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int baud=115200;const char*port="/dev/ttyS0";const char*cmd=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-b")==0&&i+1<argc)baud=atoi(argv[++i]);
        else if(strcmp(argv[i],"-p")==0&&i+1<argc)port=argv[++i];
        else if(strcmp(argv[i],"-d")==0){}
        else cmd=argv[i];
    }
    printf("Serial port: %s\n",port);
    printf("Baud rate: %d\n",baud);
    printf("Parity: none\n");
    printf("Data bits: 8\nStop bits: 1\n");
    if(cmd){printf("Running: %s (with serial redirected)\n",cmd);}
    return 0;
}
