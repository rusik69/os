/* basenc.c — encode/decode data (base64, base32, base16, base64url) */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

static const char b64[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char b64url[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char b32[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static void encode64(const unsigned char*in,unsigned long len,int url){
    const char*tab=url?b64url:b64;
    for(unsigned long i=0;i<len;i+=3){
        unsigned val=(unsigned)in[i]<<16;
        if(i+1<len)val|=in[i+1]<<8;
        if(i+2<len)val|=in[i+2];
        putchar(tab[val>>18]);putchar(tab[(val>>12)&0x3f]);
        putchar((i+1<len)?tab[(val>>6)&0x3f]:'=');
        putchar((i+2<len)?tab[val&0x3f]:'=');
    }
}
static int decode64(const char*in,unsigned long len,int url){
    const char*tab=url?b64url:b64;
    unsigned char out[1024];unsigned long oi=0;
    for(unsigned long i=0;i<len;i+=4){
        int c[4]={-1,-1,-1,-1};
        for(int j=0;j<4&&i+j<len;j++){
            char ch=in[i+j];if(ch=='=')break;
            const char*p=strchr(tab,ch);if(p)c[j]=(int)(p-tab);
        }
        if(c[0]==-1)break;
        unsigned val=(unsigned)c[0]<<18;
        if(c[1]>=0)val|=c[1]<<12;
        if(c[2]>=0)val|=c[2]<<6;
        if(c[3]>=0)val|=c[3];
        out[oi++]=val>>16;if(c[2]>=0)out[oi++]=val>>8;if(c[3]>=0)out[oi++]=val;
        if(oi+3>sizeof(out))break;
    }
    write(1,out,oi);return 0;
}
static void encode32(const unsigned char*in,unsigned long len){
    for(unsigned long i=0;i<len;i+=5){
        unsigned long val=(unsigned long)in[i]<<32;
        if(i+1<len)val|=(unsigned long)in[i+1]<<24;
        if(i+2<len)val|=(unsigned long)in[i+2]<<16;
        if(i+3<len)val|=(unsigned long)in[i+3]<<8;
        if(i+4<len)val|=in[i+4];
        putchar(b32[val>>35]);putchar(b32[(val>>30)&0x1f]);putchar(b32[(val>>25)&0x1f]);putchar(b32[(val>>20)&0x1f]);
        putchar(b32[(val>>15)&0x1f]);putchar(b32[(val>>10)&0x1f]);putchar(b32[(val>>5)&0x1f]);putchar(b32[val&0x1f]);
    }
}
static void encode16(const unsigned char*in,unsigned long len){
    for(unsigned long i=0;i<len;i++)printf("%02x",in[i]);
}

int main(int argc,char*argv[]){
    const char*fn=0;int decode=0,mode=0;/*0=base64,1=base32,2=base16,3=base64url*/
    for(int i=1;i<argc;i++){
        if(strcmp(argv[i],"--decode")==0)decode=1;
        else if(strcmp(argv[i],"--base64")==0)mode=0;
        else if(strcmp(argv[i],"--base64url")==0)mode=3;
        else if(strcmp(argv[i],"--base32")==0)mode=1;
        else if(strcmp(argv[i],"--base16")==0)mode=2;
        else fn=argv[i];
    }
    unsigned char buf[8192];unsigned long total=0;
    if(fn){int fd=open(fn,O_RDONLY,0);if(fd<0){printf("basenc: %s: No such file\n",fn);return 1;}
        int n;while((n=read(fd,buf+total,sizeof(buf)-total))>0)total+=n;close(fd);
    }else{int n;while((n=read(0,buf+total,sizeof(buf)-total))>0)total+=n;}
    if(decode){
        buf[total]=0;
        if(mode==0||mode==3)decode64((const char*)buf,total,mode==3);
        else if(mode==1){/* base32 decode not implemented */write(1,buf,total);}
        else write(1,buf,total);
    }else{
        if(mode==0||mode==3)encode64(buf,total,mode==3);
        else if(mode==1)encode32(buf,total);
        else encode16(buf,total);
    }
    return 0;
}
