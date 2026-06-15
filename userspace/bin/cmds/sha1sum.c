/* sha1sum.c — compute SHA-1 message digest */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdint.h"

#define ROTL(x,n) (((x)<<(n))|((x)>>(32-(n))))

struct sha1_ctx { uint32_t h[5]; uint64_t nbytes; };

static void sha1_init(struct sha1_ctx *c) {
    c->h[0]=0x67452301;c->h[1]=0xEFCDAB89;c->h[2]=0x98BADCFE;c->h[3]=0x10325476;c->h[4]=0xC3D2E1F0;c->nbytes=0;
}
static void sha1_transform(uint32_t h[5], const unsigned char *block) {
    uint32_t w[80],a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
    for(int i=0;i<16;i++) w[i]=(uint32_t)block[4*i]<<24|block[4*i+1]<<16|block[4*i+2]<<8|block[4*i+3];
    for(int i=16;i<80;i++) w[i]=ROTL(w[i-3]^w[i-8]^w[i-14]^w[i-16],1);
    for(int i=0;i<80;i++){
        uint32_t f,k;
        if(i<20){f=(b&c)|(~b&d);k=0x5A827999;}
        else if(i<40){f=b^c^d;k=0x6ED9EBA1;}
        else if(i<60){f=(b&c)|(b&d)|(c&d);k=0x8F1BBCDC;}
        else {f=b^c^d;k=0xCA62C1D6;}
        uint32_t tmp=ROTL(a,5)+f+e+k+w[i];e=d;d=c;c=ROTL(b,30);b=a;a=tmp;
    }
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;
}
static void sha1_update(struct sha1_ctx *c, const unsigned char *data, unsigned long len) {
    unsigned long off=(unsigned long)(c->nbytes&63);
    c->nbytes+=len;
    unsigned char buf[64];
    while(len>0){
        unsigned long chunk=(64-off<len)?64-off:len;
        if(chunk<64){memcpy(buf+off,data,chunk);off+=chunk;data+=chunk;len-=chunk;}
        else {memcpy(buf,data,64);sha1_transform(c->h,buf);off=0;data+=64;len-=64;}
        if(off>=64){sha1_transform(c->h,buf);off=0;}
    }
}
static void sha1_final(struct sha1_ctx *c, unsigned char *out) {
    uint64_t bits=c->nbytes*8;
    unsigned char pad[72]; unsigned long off=(unsigned long)(c->nbytes&63);
    pad[0]=0x80; unsigned long need=(off<56)?(56-off):(120-off);
    for(unsigned long i=1;i<=need+8;i++) pad[i]=0;
    for(int i=0;i<8;i++) pad[need+1+i]=(unsigned char)(bits>>(56-8*i));
    sha1_update(c,pad,need+1+8);
    for(int i=0;i<5;i++){out[4*i]=c->h[i]>>24;out[4*i+1]=c->h[i]>>16;out[4*i+2]=c->h[i]>>8;out[4*i+3]=c->h[i];}
}

int main(int argc,char *argv[]){
    const char *fn=0;
    for(int i=1;i<argc;i++){fn=argv[i];}
    unsigned char buf[8192]; struct sha1_ctx ctx; unsigned char digest[20];
    if(fn){int fd=open(fn,O_RDONLY,0);
        if(fd<0){printf("sha1sum: %s: No such file\n",fn);return 1;}
        sha1_init(&ctx);int n;
        while((n=read(fd,buf,sizeof(buf)))>0) sha1_update(&ctx,buf,n);
        close(fd);sha1_final(&ctx,digest);
    }else{sha1_init(&ctx);int n;while((n=read(0,buf,sizeof(buf)))>0)sha1_update(&ctx,buf,n);sha1_final(&ctx,digest);fn="-";}
    for(int i=0;i<20;i++){printf("%02x",digest[i]);}printf("  %s\n",fn);
    return 0;
}
