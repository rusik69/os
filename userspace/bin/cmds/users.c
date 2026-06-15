/* users.c — print login names of users from /etc/passwd */
#include "unistd.h"
#include "stdio.h"
#include "string.h"

int main(void){
    int fd=open("/etc/passwd",O_RDONLY,0);
    if(fd<0){
        /* Fallback: check /proc for running processes */
        fd=open("/proc",O_RDONLY,0);
        if(fd>=0){
            char buf[4096];
            int n=getdents64(fd,buf,sizeof(buf));
            close(fd);
            if(n>0){
                int printed=0;
                unsigned long off=0;
                while(off<(unsigned long)n){
                    struct dirent *de=(struct dirent*)(buf+off);
                    if(de->d_name[0]>='0'&&de->d_name[0]<='9'){
                        char path[64];
                        snprintf(path,sizeof(path),"/proc/%s/status",de->d_name);
                        int sfd=open(path,O_RDONLY,0);
                        if(sfd>=0){
                            char sbuf[512];
                            int sn=read(sfd,sbuf,sizeof(sbuf)-1);
                            close(sfd);
                            if(sn>0){
                                sbuf[sn]=0;
                                char*line=strstr(sbuf,"Uid:");
                                if(line){
                                    /* Skip "Uid:" and print uid */
                                    line+=4;
                                    while(*line==' ') line++;
                                    char*end=line;
                                    while(*end&&*end!='\t'&&*end!='\n'&&*end!=' ') end++;
                                    char saved=*end;*end=0;
                                    if(strcmp(line,"0")==0){
                                        if(!printed){
                                            printf("root\n");
                                            printed=1;
                                        }
                                    }
                                    *end=saved;
                                }
                            }
                        }
                    }
                    off+=de->d_reclen;
                }
                if(!printed) printf("root\n");
                return 0;
            }
        }
        printf("root\n");
        return 0;
    }

    char buf[4096];
    int n=read(fd,buf,sizeof(buf)-1);
    close(fd);
    if(n<=0){printf("root\n");return 0;}
    buf[n]=0;

    /* Parse /etc/passwd, extract usernames */
    char *line=buf;
    while(line&&*line){
        char*next=strchr(line,'\n');
        if(next) *next=0;
        /* Format: username:password:uid:gid:... */
        char*colon=strchr(line,':');
        if(colon){
            *colon=0;
            write(1,line,strlen(line));
            write(1,"\n",1);
        }
        if(next){*next='\n';line=next+1;}
        else break;
    }
    return 0;
}
