/* pathchk.c — check path validity and existence */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
#include "stdlib.h"

/* F_OK mode for access() */
#ifndef F_OK
#define F_OK 0
#endif

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: pathchk <path>...\n");
        return 1;
    }

    int ret=0;
    for(int i=1;i<argc;i++){
        const char *path=argv[i];
        unsigned long len=strlen(path);

        /* Check for too long paths */
        if(len>4096){
            printf("pathchk: %s: path too long (%lu)\n",path,len);
            ret=1;
            continue;
        }

        /* Check for empty path */
        if(len==0||path[0]==0){
            printf("pathchk: %s: empty path\n",path);
            ret=1;
            continue;
        }

        /* Check for invalid characters (null is the only truly invalid one) */
        for(unsigned long j=0;j<len;j++){
            if(path[j]==0){
                printf("pathchk: %s: invalid null byte in path\n",path);
                ret=1;
                break;
            }
        }

        /* Check if the path exists and is accessible */
        if(access(path,F_OK)>=0){
            printf("%s: OK (exists)\n",path);
        } else {
            /* Path doesn't exist, but the *parent* path should be valid */
            /* Check parent directory */
            char parent[4096];
            unsigned long plen=len;
            while(plen>0&&path[plen-1]=='/') plen--;
            while(plen>0&&path[plen-1]!='/') plen--;
            if(plen==0){
                strcpy(parent,"/");
            } else {
                memcpy(parent,path,plen);
                parent[plen]=0;
                /* Trim trailing slashes */
                while(plen>1&&parent[plen-1]=='/') parent[--plen]=0;
            }
            if(parent[0]&&access(parent,F_OK)>=0){
                printf("%s: OK (parent %s exists)\n",path,parent);
            } else {
                printf("pathchk: %s: no such file or directory\n",path);
                ret=1;
            }
        }
    }
    return ret;
}
