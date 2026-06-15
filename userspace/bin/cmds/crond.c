/* crond.c — cron daemon */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

/* Parse simple crontab line: min hour day month weekday command */
struct cronent { int min, hour, day, mon, wday; char cmd[256]; };
static struct cronent entries[128];static int nents;

int main(int argc,char*argv[]){
    int background=0;
    for(int i=1;i<argc;i++){if(strcmp(argv[i],"-b")==0)background=1;
        else if(strcmp(argv[i],"-f")==0)background=0;}
    int fd=open("/etc/crontab",O_RDONLY,0);
    if(fd<0){printf("crond: /etc/crontab not found\n");return 1;}
    char buf[4096];int n=read(fd,buf,sizeof(buf)-1);close(fd);buf[n]=0;
    char*line=buf;nents=0;
    while(line&&*line&&nents<128){
        char*nl=strchr(line,'\n');if(nl)*nl=0;
        if(*line!='#'&&*line!=' '&&*line!='\t'&&*line!=0){
            struct cronent*e=&entries[nents];e->min=-1;e->hour=-1;e->day=-1;e->mon=-1;e->wday=-1;
            char*cp=line;
            e->min=atoi(cp);while(*cp&&*cp!=' ')cp++;while(*cp==' ')cp++;
            if(*cp){e->hour=atoi(cp);while(*cp&&*cp!=' ')cp++;while(*cp==' ')cp++;}
            if(*cp){e->day=atoi(cp);while(*cp&&*cp!=' ')cp++;while(*cp==' ')cp++;}
            if(*cp){e->mon=atoi(cp);while(*cp&&*cp!=' ')cp++;while(*cp==' ')cp++;}
            if(*cp){e->wday=atoi(cp);while(*cp&&*cp!=' ')cp++;while(*cp==' ')cp++;}
            if(*cp){strncpy(e->cmd,cp,sizeof(e->cmd)-1);e->cmd[sizeof(e->cmd)-1]=0;nents++;}
        }
        line=nl?nl+1:0;
    }
    printf("crond: loaded %d entries from /etc/crontab\n",nents);
    if(background){printf("crond: running in background (sleep 60s loop)\n");/* In real impl: fork+loop */}else{
        printf("crond: foreground mode (one pass)\n");}
    return 0;
}
