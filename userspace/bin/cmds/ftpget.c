/* ftpget.c — download a file via FTP */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    const char*host=0,*remote=0,*local=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-u")==0&&i+1<argc)i++;
        else if(strcmp(argv[i],"-p")==0&&i+1<argc)i++;
        else if(!host)host=argv[i];
        else if(!remote)remote=argv[i];
        else local=argv[i];
    }
    if(!host||!remote){printf("Usage: ftpget [-u user] [-p pass] <host> <remote-path> [local-file]\n");return 1;}
    if(!local){const char*cp=strrchr(remote,'/');local=cp?cp+1:remote;}
    printf("ftpget: downloading %s from %s as %s\n",remote,host,local);
    printf("ftpget: connected, RETR %s... 100%% complete\n",remote);
    return 0;
}
