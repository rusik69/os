/* dircolors.c — print LS_COLORS from environment or /etc/DIR_COLORS */
#include "unistd.h"
#include "string.h"

static int read_file(const char *path, char *buf, unsigned long size) {
    int fd=open(path,O_RDONLY,0);
    if(fd<0) return -1;
    int n=read(fd,buf,size-1);
    close(fd);
    if(n>0) buf[n]=0;
    return n;
}

int main(int argc,char*argv[]){
    (void)argc;(void)argv;

    /* Try to read LS_COLORS from /proc/self/environ */
    char env[4096];
    int n=read_file("/proc/self/environ",env,sizeof(env));
    if(n>0){
        char *p=env;
        while(p<env+n){
            if(strncmp(p,"LS_COLORS=",10)==0){
                write(1,p,strlen(p));
                write(1,"\n",1);
                return 0;
            }
            p+=strlen(p)+1;
        }
    }

    /* Try /etc/DIR_COLORS */
    char buf[4096];
    n=read_file("/etc/DIR_COLORS",buf,sizeof(buf));
    if(n>0){
        write(1,buf,n);
        return 0;
    }

    /* Default LS_COLORS */
    const char*c="LS_COLORS='rs=0:di=01;34:ln=01;36:mh=00:pi=40;33:so=01;35:"
                  "do=01;35:bd=40;33;01:cd=40;33;01:or=40;31;01:mi=00:su=37;41:"
                  "sg=30;43:ca=30;41:tw=30;42:ow=34;42:st=37;44:ex=01;32:"
                  "*.tar=01;31:*.tgz=01;31:*.gz=01;31:*.bz2=01;31:*.xz=01;31:"
                  "*.zip=01;31:*.7z=01;31:*.png=01;35:*.jpg=01;35:*.jpeg=01;35:"
                  "*.gif=01;35:*.mp3=01;36:*.wav=01;36:*.mp4=01;36:'\n";
    write(1,c,strlen(c));
    return 0;
}
