/* id.c — print user identity with name lookups */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static char *get_username(int uid) {
    int fd=open("/etc/passwd",O_RDONLY,0);
    if(fd<0) return 0;
    char buf[4096];
    int n=read(fd,buf,sizeof(buf)-1);
    close(fd);
    if(n<=0) return 0;
    buf[n]=0;
    char *line=buf;
    while(line&&*line){
        char*next=strchr(line,'\n');
        if(next) *next=0;
        char*colon=strchr(line,':');
        if(!colon){if(next){*next='\n';line=next+1;}else break;continue;}
        *colon=0;
        /* Find uid field (3rd field) */
        char *p=colon+1;
        int field=1;
        while(field<2&&p){
            p=strchr(p,':');if(p){p++;field++;}
        }
        if(p){
            int fuid=atoi(p);
            if(fuid==uid){
                char *ret=malloc(strlen(line)+1);
                if (!ret) return 0;
                strcpy(ret,line);
                return ret;
            }
        }
        if(next){*next='\n';line=next+1;}else break;
    }
    return 0;
}

static char *get_groupname(int gid) {
    int fd=open("/etc/group",O_RDONLY,0);
    if(fd<0) return 0;
    char buf[4096];
    int n=read(fd,buf,sizeof(buf)-1);
    close(fd);
    if(n<=0) return 0;
    buf[n]=0;
    char *line=buf;
    while(line&&*line){
        char*next=strchr(line,'\n');
        if(next) *next=0;
        char*colon=strchr(line,':');
        if(!colon){if(next){*next='\n';line=next+1;}else break;continue;}
        *colon=0;
        char *p=colon+1;
        int field=1;
        while(field<2&&p){
            p=strchr(p,':');if(p){p++;field++;}
        }
        if(p){
            int fgid=atoi(p);
            if(fgid==gid){
                char *ret=malloc(strlen(line)+1);
                if (!ret) return 0;
                strcpy(ret,line);
                return ret;
            }
        }
        if(next){*next='\n';line=next+1;}else break;
    }
    return 0;
}

int main(void){
    int uid=getuid();
    int euid=geteuid();
    int gid=getgid();
    int egid=getegid();

    char *uname=get_username(uid);
    char *euname=get_username(euid);
    char *gname=get_groupname(gid);
    char *egname=get_groupname(egid);

    printf("uid=%d(%s) gid=%d(%s) ",uid,uname?uname:"unknown",gid,gname?gname:"unknown");
    printf("euid=%d(%s) egid=%d(%s)\n",euid,euname?euname:"unknown",egid,egname?egname:"unknown");

    if(uname) free(uname);
    if(euname) free(euname);
    if(gname) free(gname);
    if(egname) free(egname);

    return 0;
}
