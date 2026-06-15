/* b2sum.c — BLAKE2b message digest */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "stdint.h"

#define ROTR64(x,n) (((x)>>(n))|((x)<<(64-(n))))
#define B2B_G(a,b,c,d,x,y) do{\
    a=a+b+x; d=ROTR64(d^a,32); c=c+d; b=ROTR64(b^c,24);\
    a=a+b+y; d=ROTR64(d^a,16); c=c+d; b=ROTR64(b^c,63);}while(0)
static const uint64_t IV[8]={0x6a09e667f3bcc908ULL,0xbb67ae8584caa73bULL,0x3c6ef372fe94f82bULL,0xa54ff53a5f1d36f1ULL,0x510e527fade682d1ULL,0x9b05688c2b3e6c1fULL,0x1f83d9abfb41bd6bULL,0x5be0cd19137e2179ULL};
static const unsigned char sigma[12][16]={
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},{7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},{2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},{13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},{10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},{14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3}
};

struct b2b_ctx{uint64_t h[8];uint64_t t[2];unsigned long buflen;unsigned char buf[128];unsigned char outlen;};
static void b2b_init(struct b2b_ctx*c,unsigned char outlen){memcpy(c->h,IV,64);c->h[0]^=0x01010000^outlen;c->t[0]=0;c->t[1]=0;c->buflen=0;c->outlen=outlen;}
static void b2b_compress(struct b2b_ctx*c,int last){
    uint64_t v[16],m[16];memcpy(v,c->h,64);memcpy(v+8,IV,64);v[12]^=c->t[0];v[13]^=c->t[1];if(last)v[14]=~v[14];
    for(int i=0;i<16;i++){unsigned long off=8*i;m[i]=(uint64_t)c->buf[off]|(uint64_t)c->buf[off+1]<<8|(uint64_t)c->buf[off+2]<<16|(uint64_t)c->buf[off+3]<<24|(uint64_t)c->buf[off+4]<<32|(uint64_t)c->buf[off+5]<<40|(uint64_t)c->buf[off+6]<<48|(uint64_t)c->buf[off+7]<<56;}
    for(int r=0;r<12;r++){B2B_G(v[0],v[4],v[8],v[12],m[sigma[r][0]],m[sigma[r][1]]);B2B_G(v[1],v[5],v[9],v[13],m[sigma[r][2]],m[sigma[r][3]]);B2B_G(v[2],v[6],v[10],v[14],m[sigma[r][4]],m[sigma[r][5]]);B2B_G(v[3],v[7],v[11],v[15],m[sigma[r][6]],m[sigma[r][7]]);
        B2B_G(v[0],v[5],v[10],v[15],m[sigma[r][8]],m[sigma[r][9]]);B2B_G(v[1],v[6],v[11],v[12],m[sigma[r][10]],m[sigma[r][11]]);B2B_G(v[2],v[7],v[8],v[13],m[sigma[r][12]],m[sigma[r][13]]);B2B_G(v[3],v[4],v[9],v[14],m[sigma[r][14]],m[sigma[r][15]]);}
    for(int i=0;i<8;i++)c->h[i]^=v[i]^v[i+8];
}
static void b2b_update(struct b2b_ctx*c,const unsigned char*data,unsigned long len){
    while(len>0){unsigned long space=128-c->buflen;unsigned long take=(len<space)?len:space;
        memcpy(c->buf+c->buflen,data,take);c->buflen+=take;data+=take;len-=take;
        if(c->buflen==128){c->t[0]+=1024;if(c->t[0]<1024)c->t[1]++;b2b_compress(c,0);c->buflen=0;}}
}
static void b2b_final(struct b2b_ctx*c,unsigned char*out){
    c->t[0]+=c->buflen*8;if(c->t[0]<c->buflen*8)c->t[1]++;memset(c->buf+c->buflen,0,128-c->buflen);b2b_compress(c,1);
    for(unsigned long i=0;i<c->outlen;i++)for(int j=0;j<8;j++)if(i<8){out[i]=c->h[i>>3]>>(8*(i&7));break;}
    memcpy(out,c->h,c->outlen);
}

int main(int argc,char*argv[]){
    int bits=512;const char*fn=0;
    for(int i=1;i<argc;i++){if(strcmp(argv[i],"-l")==0&&i+1<argc){bits=atoi(argv[++i]);if(bits<8)bits=512;}else fn=argv[i];}
    unsigned char outlen=(unsigned char)(bits/8);if(outlen<1||outlen>64)outlen=64;
    unsigned char buf[8192];struct b2b_ctx ctx;unsigned char digest[64];
    if(fn){int fd=open(fn,O_RDONLY,0);if(fd<0){printf("b2sum: %s: No such file\n",fn);return 1;}
        b2b_init(&ctx,outlen);int n;while((n=read(fd,buf,sizeof(buf)))>0)b2b_update(&ctx,buf,n);close(fd);b2b_final(&ctx,digest);
    }else{b2b_init(&ctx,outlen);int n;while((n=read(0,buf,sizeof(buf)))>0)b2b_update(&ctx,buf,n);b2b_final(&ctx,digest);fn="-";}
    for(int i=0;i<outlen;i++){printf("%02x",digest[i]);}printf("  %s\n",fn);return 0;
}