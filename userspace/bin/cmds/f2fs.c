/* f2fs.c — F2FS filesystem utility */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: f2fs <device>\n");return 1;}
    printf("F2FS filesystem on %s\n",argv[1]);
    int fd=open(argv[1],O_RDONLY,0);
    if(fd<0){printf("f2fs: cannot open %s\n",argv[1]);return 1;}
    unsigned char super[4096];read(fd,super,sizeof(super));close(fd);
    unsigned magic=(unsigned)super[0]|(unsigned)super[1]<<8|(unsigned)super[2]<<16|(unsigned)super[3]<<24;
    printf("  Magic: 0x%08x%s\n",magic,magic==0xF2F52010?" (F2FS)":" (unknown)");
    return 0;
}
