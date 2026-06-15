/* tr.c — translate or delete characters */
#include "unistd.h"
#include "string.h"
#include "stdio.h"
int main(int argc,char*argv[]){
    if(argc<2||(argc==2&&strcmp(argv[1],"-d")!=0)){
        char buf[4096];long n;
        while((n=read(0,buf,sizeof(buf)))>0)write(1,buf,n);
        return 0;
    }
    if(argc==3&&strcmp(argv[1],"-d")==0){
        char buf[4096];long n;
        const char*set=argv[2];unsigned long slen=strlen(set);
        while((n=read(0,buf,sizeof(buf)))>0){
            int wp=0;
            for(long i=0;i<n;i++){
                int found=0;
                for(unsigned long j=0;j<slen;j++){if(buf[i]==set[j]){found=1;break;}}
                if(!found)buf[wp++]=buf[i];
            }
            if(wp>0)write(1,buf,wp);
        }
    } else if(argc==3){
        char buf[4096];long n;
        const char*from=argv[1],*to=argv[2];
        unsigned long flen=strlen(from);
        while((n=read(0,buf,sizeof(buf)))>0){
            for(long i=0;i<n;i++){
                for(unsigned long j=0;j<flen;j++){
                    if(buf[i]==from[j]){buf[i]=to[j];break;}
                }
            }
            write(1,buf,n);
        }
    } else {
        char buf[4096];long n;
        while((n=read(0,buf,sizeof(buf)))>0)write(1,buf,n);
    }
    return 0;
}
