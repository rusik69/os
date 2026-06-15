/* mesg.c — control write access to terminal */
#include "unistd.h"
#include "string.h"
#include "stdio.h"

int main(int argc,char*argv[]){
    /* Find our tty device path */
    char tty_path[256];
    int n=readlinkat(AT_FDCWD,"/proc/self/fd/0",tty_path,sizeof(tty_path)-1);
    if(n<0){
        strcpy(tty_path,"/dev/ttyS0");
    } else {
        tty_path[n]=0;
    }

    if(argc>1){
        if(strcmp(argv[1],"y")==0){
            /* Allow write access: chmod o+w */
            if(chmod(tty_path,0622)>=0){
                printf("mesg: write access enabled for %s\n",tty_path);
            } else {
                printf("mesg: cannot enable write access\n");
                return 1;
            }
            return 0;
        }
        if(strcmp(argv[1],"n")==0){
            /* Deny write access: chmod o-w */
            if(chmod(tty_path,0600)>=0){
                printf("mesg: write access disabled for %s\n",tty_path);
            } else {
                printf("mesg: cannot disable write access\n");
                return 1;
            }
            return 0;
        }
        printf("Usage: mesg [y|n]\n");
        return 1;
    }

    /* Default: show current state */
    struct stat st;
    if(stat(tty_path,&st)>=0){
        if(st.st_mode & 2){ /* S_IWOTH = 2 */
            printf("is y\n");
        } else {
            printf("is n\n");
        }
    } else {
        printf("is y\n");
    }
    return 0;
}
