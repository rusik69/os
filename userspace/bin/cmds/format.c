/* format.c — format filesystem (multi-fs dispatcher) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){
        printf("Usage: format <device> [fstype]\n");
        printf("  fstype: ext2, vfat, smfs (default)\n");
        return 1;
    }

    const char*dev=argv[1];
    const char*fstype=argc>2?argv[2]:"smfs";

    printf("format: creating %s filesystem on %s\n",fstype,dev);

    /* Dispatch to appropriate mkfs helper */
    char cmd[256];
    if(strcmp(fstype,"ext2")==0||strcmp(fstype,"ext3")==0||strcmp(fstype,"ext4")==0){
        snprintf(cmd,sizeof(cmd),"/bin/mkfs_ext2");
    } else if(strcmp(fstype,"vfat")==0||strcmp(fstype,"msdos")==0||strcmp(fstype,"fat")==0){
        snprintf(cmd,sizeof(cmd),"/bin/mkdosfs");
    } else if(strcmp(fstype,"smfs")==0){
        /* SMFS format: write a minimal superblock and clear the rest */
        int fd=open(dev,O_WRONLY,0);
        if(fd<0){
            printf("format: cannot open '%s'\n",dev);
            return 1;
        }
        /* Write SMFS signature at offset 0 */
        unsigned char sb[1024];
        memset(sb,0,sizeof(sb));
        memcpy(sb,"SMFS",4);
        /* Block size: 4096, total blocks computed from size */
        unsigned long long total_blocks=0;
        struct stat st;
        if(fstat(fd,&st)>=0&&st.st_size>0){
            total_blocks=st.st_size/4096;
        } else {
            total_blocks=65536; /* default 256MB */
        }
        sb[4]=(unsigned char)(total_blocks&0xff);
        sb[5]=(unsigned char)((total_blocks>>8)&0xff);
        sb[6]=(unsigned char)((total_blocks>>16)&0xff);
        sb[7]=(unsigned char)((total_blocks>>24)&0xff);
        write(fd,sb,sizeof(sb));
        close(fd);
        printf("format: SMFS filesystem created (%llu blocks)\n",total_blocks);
        return 0;
    } else {
        printf("format: unsupported filesystem '%s'\n",fstype);
        return 1;
    }

    /* Fork+exec the helper */
    int pid=fork();
    if(pid<0){printf("format: fork failed\n");return 1;}
    if(pid==0){
        char *args[]={cmd,(char*)dev,NULL};
        execve(args[0],args,0);
        printf("format: cannot exec helper\n");
        exit(1);
    }
    int status;
    waitpid(pid,&status,0);
    return status;
}
