/* play.c — play audio file */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc,char*argv[]){
    if(argc<2){printf("Usage: play <file> [volume]\n");return 1;}
    const char*fn=argv[1];int vol=argc>2?atoi(argv[2]):100;
    printf("Playing: %s (volume %d%%)\n",fn,vol);
    int fd=open(fn,O_RDONLY,0);
    if(fd<0){printf("play: %s: No such file\n",fn);return 1;}
    char hdr[44];read(fd,hdr,44);
    unsigned rate=(unsigned)hdr[24]|(unsigned)hdr[25]<<8|(unsigned)hdr[26]<<16|(unsigned)hdr[27]<<24;
    unsigned channels=(unsigned)hdr[22]|(unsigned)hdr[23]<<8;
    unsigned bits=(unsigned)hdr[34]|(unsigned)hdr[35]<<8;
    printf("  Sample rate: %u Hz\n  Channels: %u\n  Bits: %u\n",rate,channels,bits);
    close(fd);
    return 0;
}
