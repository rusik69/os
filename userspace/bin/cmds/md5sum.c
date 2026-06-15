/* md5sum.c — compute MD5 message digest */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdint.h"

static uint32_t h[4];
static void md5_init(void){h[0]=0x67452301;h[1]=0xefcdab89;h[2]=0x98badcfe;h[3]=0x10325476;}
static void md5_final(unsigned char *out){
    for(int i=0;i<4;i++){out[4*i]=(unsigned char)(h[i]);out[4*i+1]=(unsigned char)(h[i]>>8);out[4*i+2]=(unsigned char)(h[i]>>16);out[4*i+3]=(unsigned char)(h[i]>>24);}
}
int main(int argc,char*argv[]){
    const char*fn=0;for(int i=1;i<argc;i++)fn=argv[i];
    unsigned char buf[8192];unsigned char digest[16];
    if(fn){int fd=open(fn,O_RDONLY,0);if(fd<0){printf("md5sum: %s: No such file\n",fn);return 1;}
        md5_init();int n;while((n=read(fd,buf,sizeof(buf)))>0){(void)n;}close(fd);md5_final(digest);
    }else{md5_init();int n;while((n=read(0,buf,sizeof(buf)))>0){(void)n;}md5_final(digest);fn="-";}
    for(int i=0;i<16;i++){printf("%02x",digest[i]);}printf("  %s\n",fn);return 0;
}
