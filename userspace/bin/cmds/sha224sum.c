/* sha224sum.c — compute SHA-224 message digest */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdint.h"
#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define BSIG0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define BSIG1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SSIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SSIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))
static const uint32_t K[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
struct sha224_ctx{uint32_t h[8];uint64_t nbytes;};
static void sha224_init(struct sha224_ctx*c){c->h[0]=0xc1059ed8;c->h[1]=0x367cd507;c->h[2]=0x3070dd17;c->h[3]=0xf70e5939;c->h[4]=0xffc00b31;c->h[5]=0x68581511;c->h[6]=0x64f98fa7;c->h[7]=0xbefa4fa4;c->nbytes=0;}
static void sha224_transform(uint32_t h[8],const unsigned char*block){
    uint32_t w[64],a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],h2=h[7];
    for(int i=0;i<16;i++)w[i]=(uint32_t)block[4*i]<<24|block[4*i+1]<<16|block[4*i+2]<<8|block[4*i+3];
    for(int i=16;i<64;i++)w[i]=SSIG1(w[i-2])+w[i-7]+SSIG0(w[i-15])+w[i-16];
    for(int i=0;i<64;i++){uint32_t t1=h2+BSIG1(e)+CH(e,f,g)+K[i]+w[i];uint32_t t2=BSIG0(a)+MJ(a,b,c);h2=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
    h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=h2;
}
static void sha224_update(struct sha224_ctx*c,const unsigned char*data,unsigned long len){
    unsigned long off=(unsigned long)(c->nbytes&63);c->nbytes+=len;
    while(len>0){unsigned long chunk=(64-off<len)?64-off:len;if(chunk<64){memcpy((unsigned char*)c->h+off,data,chunk);off+=chunk;data+=chunk;len-=chunk;}
        else{unsigned char buf[64];memcpy(buf,data,64);sha224_transform(c->h,buf);off=0;data+=64;len-=64;}}
}
static void sha224_final(struct sha224_ctx*c,unsigned char*out){
    uint64_t bits=c->nbytes*8;unsigned char pad[72];unsigned long off=(unsigned long)(c->nbytes&63);
    pad[0]=0x80;unsigned long need=(off<56)?(56-off):(120-off);
    for(unsigned long i=1;i<=need+8;i++)pad[i]=0;
    for(int i=0;i<8;i++)pad[need+1+i]=(unsigned char)(bits>>(56-8*i));
    sha224_update(c,pad,need+1+8);
    for(int i=0;i<7;i++){out[4*i]=c->h[i]>>24;out[4*i+1]=c->h[i]>>16;out[4*i+2]=c->h[i]>>8;out[4*i+3]=c->h[i];}
}
int main(int argc,char*argv[]){const char*fn=0;for(int i=1;i<argc;i++)fn=argv[i];
    unsigned char buf[8192];struct sha224_ctx ctx;unsigned char digest[28];
    if(fn){int fd=open(fn,O_RDONLY,0);if(fd<0){printf("sha224sum: %s: No such file\n",fn);return 1;}
        sha224_init(&ctx);int n;while((n=read(fd,buf,sizeof(buf)))>0)sha224_update(&ctx,buf,n);close(fd);sha224_final(&ctx,digest);
    }else{sha224_init(&ctx);int n;while((n=read(0,buf,sizeof(buf)))>0)sha224_update(&ctx,buf,n);sha224_final(&ctx,digest);fn="-";}
    for(int i=0;i<28;i++){printf("%02x",digest[i]);}printf("  %s\n",fn);return 0;}

