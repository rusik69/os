/* dir.c — list directory contents using getdents64 */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    const char*dir=argc>1?argv[1]:".";
    int fd=open(dir,O_RDONLY,0);
    if(fd<0){
        printf("dir: %s: No such directory\n",dir);
        return 1;
    }

    char buf[8192];
    int n=getdents64(fd,buf,sizeof(buf));
    close(fd);

    if(n<0){
        printf("dir: %s: error reading directory\n",dir);
        return 1;
    }

    printf("Directory: %s\n",dir);
    printf("---\n");

    if(n==0){
        printf("(empty)\n");
        return 0;
    }

    unsigned long off=0;
    while(off<(unsigned long)n){
        struct dirent *de=(struct dirent*)(buf+off);
        if(de->d_name[0]!='\0'){
            char type=' ';
            switch(de->d_type){
                case 4: type='d'; break; /* DT_DIR */
                case 8: type='-'; break; /* DT_REG */
                case 10: type='l'; break; /* DT_LNK */
                case 2: type='c'; break; /* DT_CHR */
                case 6: type='b'; break; /* DT_BLK */
                default: type='?';
            }
            printf("%c %s\n",type,de->d_name);
        }
        off+=de->d_reclen;
    }
    return 0;
}
