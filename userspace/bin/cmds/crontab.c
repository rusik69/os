/* crontab.c — cron table manager */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    int edit=0,list=0,remove=0;
    const char*user=0,*file=0;
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-e")==0)edit=1;
        else if(strcmp(argv[i],"-l")==0)list=1;
        else if(strcmp(argv[i],"-r")==0)remove=1;
        else if(strcmp(argv[i],"-u")==0&&i+1<argc)user=argv[++i];
        else file=argv[i];
    }
    if(list){printf("# Crontab for %s\n",user?user:"root");printf("MIN HOUR DAY MONTH WEEKDAY COMMAND\n");}
    if(edit){printf("crontab: editing crontab for %s\n",user?user:"root");printf("crontab: set EDITOR or use 'crontab -l > file; $EDITOR file; crontab file'\n");}
    if(remove){printf("crontab: removed crontab for %s\n",user?user:"root");}
    if(file){printf("crontab: installing %s for %s\n",file,user?user:"root");}
    if(!edit&&!list&&!remove&&!file)printf("Usage: crontab [-e] [-l] [-r] [-u user] [file]\n");
    return 0;
}
