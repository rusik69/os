/* serial.c — serial port configuration and data transfer */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int baud=115200;
    const char*port="/dev/ttyS0";
    const char*cmd=0;
    int dump=0;

    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-b")==0&&i+1<argc) baud=atoi(argv[++i]);
        else if(strcmp(argv[i],"-p")==0&&i+1<argc) port=argv[++i];
        else if(strcmp(argv[i],"-d")==0) dump=1;
        else if(strcmp(argv[i],"--help")==0||strcmp(argv[i],"-h")==0){
            printf("Usage: serial [-b BAUD] [-p PORT] [-d] [command]\n");
            printf("  -b BAUD   Baud rate (default: 115200)\n");
            printf("  -p PORT   Serial port (default: /dev/ttyS0)\n");
            printf("  -d        Dump raw data from serial port\n");
            printf("  command   Run command with serial redirected\n");
            return 0;
        } else cmd=argv[i];
    }

    printf("Serial port:  %s\n",port);
    printf("Baud rate:    %d\n",baud);
    printf("Parity:       none\n");
    printf("Data bits:    8\n");
    printf("Stop bits:    1\n");
    printf("Flow control: none\n");

    /* Try to open the port */
    int fd=open(port,O_RDWR,0);
    if(fd>=0){
        printf("Port opened successfully\n");

        if(dump){
            /* Dump raw data */
            printf("Dumping data from %s (Ctrl-C to stop)...\n",port);
            char buf[256];
            int n;
            while((n=read(fd,buf,sizeof(buf)))>0){
                write(1,buf,n);
            }
            printf("Serial port closed\n");
        }

        /* If command specified, redirect stdin/stdout to serial port */
        if(cmd){
            printf("Running: %s (with serial redirected)\n",cmd);
            dup2(fd,0);
            dup2(fd,1);
            dup2(fd,2);
            close(fd);
            execve(cmd,argv+(int)(cmd-argv[0]),0);
            printf("serial: cannot exec %s\n",cmd);
            return 1;
        }

        close(fd);
    } else {
        printf("Warning: could not open %s (port may not exist)\n",port);
        if(cmd){
            printf("Running: %s (without serial redirection)\n",cmd);
            execve(cmd,argv+(int)(cmd-argv[0]),0);
            printf("serial: cannot exec %s\n",cmd);
            return 1;
        }
    }

    return 0;
}
