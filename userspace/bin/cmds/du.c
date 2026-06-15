/* du.c — disk usage with recursive support */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "sys/stat.h"

static void format_size(unsigned long long bytes, char *buf, unsigned long size) {
    if(bytes>=1073741824ULL)
        snprintf(buf,size,"%.1fG",(double)bytes/1073741824.0);
    else if(bytes>=1048576ULL)
        snprintf(buf,size,"%.1fM",(double)bytes/1048576.0);
    else if(bytes>=1024ULL)
        snprintf(buf,size,"%.1fK",(double)bytes/1024.0);
    else
        snprintf(buf,size,"%llu",bytes);
}

static unsigned long long du_path(const char *path, int show_size, int max_depth, int depth) {
    struct stat st;
    if(stat(path,&st)<0){
        if(depth==0) printf("du: %s: error\n",path);
        return 0;
    }

    unsigned long long total=st.st_blocks*512;

    if(S_ISDIR(st.st_mode)){
        int fd=open(path,O_RDONLY,0);
        if(fd>=0){
            char buf[8192];
            int n=getdents64(fd,buf,sizeof(buf));
            close(fd);
            if(n>0){
                unsigned long off=0;
                while(off<(unsigned long)n){
                    struct dirent *de=(struct dirent*)(buf+off);
                    if(de->d_name[0]!='.'||(de->d_name[1]!='\0'&&!(de->d_name[1]=='.'&&de->d_name[2]=='\0'))){
                        char child[1024];
                        int plen=strlen(path);
                        if(plen+1+strlen(de->d_name)>=sizeof(child)){off+=de->d_reclen;continue;}
                        memcpy(child,path,plen);
                        child[plen]='/';
                        strcpy(child+plen+1,de->d_name);
                        total+=du_path(child,0,max_depth,depth+1);
                    }
                    off+=de->d_reclen;
                }
            }
        }
    }

    if(show_size||depth<=max_depth){
        char sz[32];
        format_size(total,sz,sizeof(sz));
        printf("%s\t%s\n",sz,path);
    }
    return total;
}

int main(int argc, char *argv[]) {
    int show_summary=0, human=0;
    const char *path=".";
    int max_depth=999999;

    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"-s")==0) show_summary=1;
        else if(strcmp(argv[i],"-h")==0) human=1;
        else if(strcmp(argv[i],"-d")==0&&i+1<argc) max_depth=atoi(argv[++i]);
        else if(argv[i][0]!='-') path=argv[i];
    }

    if(show_summary){
        unsigned long long total=du_path(path,0,0,0);
        char sz[32];
        if(human) format_size(total,sz,sizeof(sz));
        else snprintf(sz,sizeof(sz),"%llu",total/1024);
        printf("%s\t%s\n",sz,path);
    } else {
        du_path(path,1,max_depth,0);
    }

    return 0;
}
