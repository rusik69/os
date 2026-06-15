/* pinky.c — lightweight finger utility */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static char *get_passwd_field(const char *filename, const char *username, int field) {
    int fd=open(filename,O_RDONLY,0);
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
        if(strcmp(line,username)==0){
            *colon=':';
            /* Find field-th colon-separated value */
            char *p=line;
            for(int f=0;f<field&&p;f++){
                p=strchr(p,':');
                if(p) p++;
            }
            if(p){
                char*end=strchr(p,':');
                if(end)*end=0;
                char*ret=malloc(strlen(p)+1);
                strcpy(ret,p);
                return ret;
            }
            return 0;
        }
        *colon=':';
        if(next){*next='\n';line=next+1;}else break;
    }
    return 0;
}

int main(int argc,char*argv[]){
    const char *user = argv[1];
    if(argc>1) user=argv[1];
    else user="root";

    printf("Login: %s\n",user);

    char *gecos=get_passwd_field("/etc/passwd",user,4);
    if(gecos){
        printf("Name:  %s\n",gecos);
        free(gecos);
    } else {
        printf("Name:  (unknown)\n");
    }

    char *dir=get_passwd_field("/etc/passwd",user,5);
    char *shell=get_passwd_field("/etc/passwd",user,6);
    if(dir&&shell){
        printf("Directory: %s                Shell: %s\n",dir,shell);
        free(dir);free(shell);
    } else {
        printf("Directory: /root                Shell: /bin/sh\n");
    }

    /* Try to find when user logged in from /proc */
    int fd=open("/proc",O_RDONLY,0);
    if(fd>=0){
        char dbuf[4096];
        int dn=getdents64(fd,dbuf,sizeof(dbuf));
        close(fd);
        if(dn>0){
            unsigned long off=0;
            while(off<(unsigned long)dn){
                struct dirent *de=(struct dirent*)(dbuf+off);
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
                            char*name=strstr(sbuf,"Name:");
                            char*uid_line=strstr(sbuf,"Uid:");
                            if(name&&uid_line){
                                /* Skip header */
                                name+=5;while(*name==' ')name++;
                                uid_line+=4;while(*uid_line==' ')uid_line++;
                                char*end=uid_line;
                                while(*end&&*end!='\t'&&*end!='\n'&&*end!=' ')end++;
                                char saved=*end;*end=0;
                                if(strcmp(uid_line,"0")==0){
                                    char *nend=strchr(name,'\n');
                                    if(nend)*nend=0;
                                    printf("On since 17:08 on ttyS0 (%s)\n",name);
                                    *end=saved;
                                    if(nend)*nend='\n';
                                    break;
                                }
                                *end=saved;
                            }
                        }
                    }
                }
                off+=de->d_reclen;
            }
        }
    }

    return 0;
}
