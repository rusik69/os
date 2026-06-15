/* groups.c — print group membership */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(void){
    int gid=getgid();

    /* Try to read /etc/group to find groups */
    int fd=open("/etc/group",O_RDONLY,0);
    if(fd>=0){
        char buf[4096];
        int n=read(fd,buf,sizeof(buf)-1);
        close(fd);
        if(n>0){
            buf[n]=0;
            char *line=buf;
            while(line&&*line){
                char*next=strchr(line,'\n');
                if(next) *next=0;
                /* Format: group:password:gid:userlist */
                char *gname=line;
                char*colon=strchr(line,':');
                if(colon){
                    *colon=0;
                    /* Check if current user is in this group */
                    char *gid_end=colon+1;
                    char *gid_str=gid_end;
                    gid_end=strchr(gid_str,':');
                    if(gid_end){
                        char *users=gid_end+1;
                        int matched=0;
                        if(atoi(gid_str)==gid) matched=1;
                        if(strstr(users,"root")||strstr(users,",root")||strstr(users,"root,")) matched=1;
                        if(matched){
                            write(1,gname,strlen(gname));
                            write(1," ",1);
                        }
                    }
                }
                if(next){*next='\n';line=next+1;}else break;
            }
            write(1,"\n",1);
            return 0;
        }
    }

    /* Fallback */
    printf("root\n");
    return 0;
}
