/* locale.c — show locale from environment */
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

static void print_env_var(const char *name) {
    char env[4096];
    int n=read_file("/proc/self/environ",env,sizeof(env));
    if(n>0){
        char *p=env;
        while(p<env+n){
            unsigned long plen=strlen(p);
            if(strncmp(p,name,strlen(name))==0&&p[strlen(name)]=='='){
                write(1,p,plen);
                write(1,"\n",1);
                return;
            }
            p+=plen+1;
        }
    }
    /* Default */
    write(1,name,strlen(name));
    write(1,"=POSIX\n",7);
}

int main(int argc,char*argv[]){
    (void)argc;(void)argv;

    print_env_var("LANG");
    print_env_var("LC_CTYPE");
    print_env_var("LC_NUMERIC");
    print_env_var("LC_TIME");
    print_env_var("LC_COLLATE");
    print_env_var("LC_MONETARY");
    print_env_var("LC_MESSAGES");
    print_env_var("LC_ALL");
    return 0;
}
