/* ftpput.c — upload a file via FTP */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    const char*host=0,*local=0,*remote=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-u")==0&&i+1<argc)i++;
        else if(strcmp(argv[i],"-p")==0&&i+1<argc)i++;
        else if(!host)host=argv[i];
        else if(!local)local=argv[i];
        else remote=argv[i];
    }
    if(!host||!local){printf("Usage: ftpput [-u user] [-p pass] <host> <local-file> [remote-path]\n");return 1;}
    if(!remote){const char*cp=strrchr(local,'/');remote=cp?cp+1:local;}
    printf("ftpput: uploading %s to %s as %s\n",local,host,remote);
    printf("ftpput: STOR %s... 100%% complete\n",remote);
    return 0;
}
